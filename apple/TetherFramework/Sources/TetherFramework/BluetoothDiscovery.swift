//
//  BluetoothDiscovery.swift
//  TetherFramework
//
//  Fallback for networks that block mDNS: reads tetherd's LAN addresses from a
//  GATT characteristic over the existing Bluetooth bond. Bluetooth only supplies
//  an address; mTLS pinning still decides trust. Contract in docs/PROTOCOL.md.
//

import CoreBluetooth
import Foundation
import Network

// The characteristic's value: `{"v":1,"name":…,"port":…,"fp":…,"addrs":["ip/prefix",…]}`.
public struct LanInfo: Decodable, Sendable, Equatable {
    public let v: Int
    public let name: String
    public let port: UInt16
    public let fp: String
    public let addrs: [String]

    // Unknown keys are ignored; a different `v` is an incompatible change.
    public static func decode(_ data: Data) -> LanInfo? {
        guard let info = try? JSONDecoder().decode(LanInfo.self, from: data),
              info.v == 1, info.port > 0 else { return nil }
        return info
    }
}

// Reads `LanInfo` from every bonded, connected `tetherd`. Use from the main thread.
public final class BluetoothDiscovery: NSObject {
    public static let serviceUUID = CBUUID(string: "BA7A69F7-BF0F-4C09-8C73-CF4EE9AFB5B5")
    public static let characteristicUUID = CBUUID(string: "BA7A69F7-BF0F-4C09-8C73-CF4EE9AFB5B6")

    // Whether a lookup can run without showing the permission prompt.
    public static var isAuthorized: Bool {
        CBManager.authorization == .allowedAlways
    }

    // Created on the first lookup, since creating it is what prompts for permission.
    private var central: CBCentralManager?
    private var waiters: [CheckedContinuation<[LanInfo], Never>] = []
    private var timeout: Double = 5
    private var readsStarted = false
    private var reading: [UUID: CBPeripheral] = [:]
    private var collected: [LanInfo] = []
    // Bumped per lookup so a stale timeout cannot end the next one.
    private var generation = 0

    public override init() {
        super.init()
    }

    // MARK: - Public

    // Hosts found over Bluetooth, each dialing its best address. Concurrent calls share one
    // lookup. The timeout starts once Bluetooth is ready, not while the permission prompt is up.
    @MainActor
    public func lookup(timeout: Double = 5) async -> [DiscoveredHost] {
        let infos: [LanInfo] = await withCheckedContinuation { continuation in
            waiters.append(continuation)
            if waiters.count == 1 {
                begin(timeout: timeout)
            }
        }

        let phone = Self.phoneAddresses()
        return infos.compactMap { info in
            guard let best = Self.orderAddresses(info.addrs, phone: phone).first,
                  let port = NWEndpoint.Port(rawValue: info.port) else { return nil }
            return DiscoveredHost(
                name: info.name,
                endpoint: .hostPort(host: NWEndpoint.Host(best), port: port),
                fingerprint: info.fp
            )
        }
    }

    // Bare hosts from `addrs` ("ip/prefix"), those sharing a subnet with one of the phone's
    // interfaces first, the rest in the daemon's order. Unparseable entries are dropped.
    public static func orderAddresses(_ addrs: [String], phone: [String]) -> [String] {
        let candidates = addrs.compactMap(CIDR.init)
        let local = phone.compactMap(CIDR.init)
        let isNear = { (c: CIDR) in local.contains { c.contains($0) } }
        return (candidates.filter(isNear) + candidates.filter { !isNear($0) }).map(\.host)
    }

    // MARK: - Private — Lookup

    private func begin(timeout: Double) {
        self.timeout = timeout
        guard let central else {
            central = CBCentralManager(
                delegate: self,
                queue: .main,
                options: [CBCentralManagerOptionShowPowerAlertKey: false]
            )
            return // centralManagerDidUpdateState continues
        }
        switch central.state {
        case .unknown, .resetting:
            break // centralManagerDidUpdateState continues
        default:
            startReads()
        }
    }

    private func startReads() {
        readsStarted = true
        guard let central, central.state == .poweredOn else {
            complete()
            return
        }

        let peripherals = central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID])
        guard !peripherals.isEmpty else {
            complete()
            return
        }

        let gen = generation
        Task { @MainActor [weak self, timeout] in
            try? await Task.sleep(for: .seconds(timeout))
            guard let self, self.generation == gen else { return }
            self.complete()
        }

        for peripheral in peripherals {
            reading[peripheral.identifier] = peripheral
            peripheral.delegate = self
            central.connect(peripheral)
        }
    }

    private func finishRead(_ peripheral: CBPeripheral, value: Data?) {
        guard reading.removeValue(forKey: peripheral.identifier) != nil else { return }
        if let value, let info = LanInfo.decode(value) {
            collected.append(info)
        }
        // Drops only our use of the link; the system's bond connection stays up.
        central?.cancelPeripheralConnection(peripheral)
        if reading.isEmpty {
            complete()
        }
    }

    private func complete() {
        generation += 1
        reading.values.forEach { central?.cancelPeripheralConnection($0) }
        reading.removeAll()
        readsStarted = false

        let result = collected
        collected.removeAll()
        let pending = waiters
        waiters.removeAll()
        pending.forEach { $0.resume(returning: result) }
    }

    // MARK: - Private — Phone addresses

    // The phone's own up interfaces as "ip/prefix", without loopback or IPv6 link-local.
    static func phoneAddresses() -> [String] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }

        var result: [String] = []
        for ifa in sequence(first: first, next: { $0.pointee.ifa_next }).map(\.pointee) {
            let flags = Int32(ifa.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_LOOPBACK == 0,
                  let addr = ifa.ifa_addr, let mask = ifa.ifa_netmask else { continue }

            let family = Int32(addr.pointee.sa_family)
            guard family == AF_INET || family == AF_INET6 else { continue }

            let bytes = addressBytes(addr, family: family)
            if family == AF_INET6, bytes[0] == 0xFE, bytes[1] & 0xC0 == 0x80 { continue }

            // Netmasks can be truncated and carry no family, so read them as the address's.
            let prefix = addressBytes(mask, family: family).reduce(0) { $0 + $1.nonzeroBitCount }
            let host = family == AF_INET
                ? IPv4Address(Data(bytes)).map { "\($0)" }
                : IPv6Address(Data(bytes)).map { "\($0)" }
            if let host {
                result.append("\(host)/\(prefix)")
            }
        }
        return result
    }

    // The address bytes of a sockaddr_in/sockaddr_in6, zero past a truncated `sa_len`.
    private static func addressBytes(_ sa: UnsafeMutablePointer<sockaddr>, family: Int32) -> [UInt8] {
        let (offset, count) = family == AF_INET
            ? (MemoryLayout<sockaddr_in>.offset(of: \.sin_addr)!, 4)
            : (MemoryLayout<sockaddr_in6>.offset(of: \.sin6_addr)!, 16)
        let length = Int(sa.pointee.sa_len)
        let raw = UnsafeRawPointer(sa)
        return (0..<count).map { i in
            offset + i < length ? raw.load(fromByteOffset: offset + i, as: UInt8.self) : 0
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension BluetoothDiscovery: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard !waiters.isEmpty else { return }
        switch central.state {
        case .unknown, .resetting:
            break
        case .poweredOn:
            if !readsStarted { startReads() }
        default:
            complete()
        }
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.serviceUUID])
    }

    public func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        finishRead(peripheral, value: nil)
    }

    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        finishRead(peripheral, value: nil)
    }
}

// MARK: - CBPeripheralDelegate

extension BluetoothDiscovery: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            finishRead(peripheral, value: nil)
            return
        }
        peripheral.discoverCharacteristics([Self.characteristicUUID], for: service)
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristic = service.characteristics?.first(where: { $0.uuid == Self.characteristicUUID }) else {
            finishRead(peripheral, value: nil)
            return
        }
        // Encrypted read: an unbonded phone gets an error here, which counts as no host.
        peripheral.readValue(for: characteristic)
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        finishRead(peripheral, value: error == nil ? characteristic.value : nil)
    }
}

// MARK: - CIDR

// An "ip/prefix" string; a bare ip is a full-length prefix.
private struct CIDR {
    let host: String
    let bytes: [UInt8]
    let prefix: Int

    init?(_ string: String) {
        let parts = string.split(separator: "/", maxSplits: 1, omittingEmptySubsequences: false)
        let host = String(parts[0])
        if let v4 = IPv4Address(host) {
            bytes = [UInt8](v4.rawValue)
        } else if let v6 = IPv6Address(host) {
            bytes = [UInt8](v6.rawValue)
        } else {
            return nil
        }

        let maxPrefix = bytes.count * 8
        if parts.count == 2 {
            guard let prefix = Int(parts[1]), (0...maxPrefix).contains(prefix) else { return nil }
            self.prefix = prefix
        } else {
            prefix = maxPrefix
        }
        self.host = host
    }

    // Whether `other` falls inside this network, under this entry's prefix.
    func contains(_ other: CIDR) -> Bool {
        guard bytes.count == other.bytes.count else { return false }
        var remaining = prefix
        for i in bytes.indices where remaining > 0 {
            let bits = min(8, remaining)
            let mask = UInt8(truncatingIfNeeded: 0xFF << (8 - bits))
            if bytes[i] & mask != other.bytes[i] & mask { return false }
            remaining -= bits
        }
        return true
    }
}
