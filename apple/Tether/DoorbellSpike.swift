//
//  DoorbellSpike.swift
//  Tether
//
//  SPIKE (#336 Phase 0, not for merge). Holds an LE connection to tetherd,
//  subscribes to the clipboard ring characteristic (…b7), and on each notify
//  writes "ring <time>" to the pasteboard, then pulls the clipboard over mTLS
//  and writes that. Every step goes to Documents/doorbell.log (visible in Files)
//  so background and relaunch behaviour can be read back afterwards.
//

import CoreBluetooth
import os
import TetherFramework
import UIKit

final class DoorbellSpike: NSObject {
    static let shared = DoorbellSpike()

    static let restoreIdentifier = "com.tether.doorbell-spike"
    static let ringUUID = CBUUID(string: "BA7A69F7-BF0F-4C09-8C73-CF4EE9AFB5B7")
    private static let peripheralKey = "DoorbellSpikePeripheral"

    private var central: CBCentralManager!
    private var held: [UUID: CBPeripheral] = [:]
    private var pulling = false
    private var ringDuringPull = false
    // Pulled text the pasteboard refused (phone locked); retried when data unlocks.
    private var pendingText: String?

    private let logger = Logger(subsystem: "com.tether", category: "doorbell-spike")

    // Must run in didFinishLaunching so state restoration finds the manager.
    func start(launchOptions: [UIApplication.LaunchOptionsKey: Any]?) {
        let state = UIApplication.shared.applicationState
        let restored = launchOptions?[.bluetoothCentrals] as? [String]
        log("launch state=\(Self.describe(state)) bluetoothCentrals=\(restored ?? []) protected=\(UIApplication.shared.isProtectedDataAvailable) debugger=\(Self.debuggerAttached())")
        let center = NotificationCenter.default
        center.addObserver(forName: UIApplication.protectedDataDidBecomeAvailableNotification, object: nil, queue: .main) { [weak self] _ in
            self?.flushPending(reason: "protectedDataDidBecomeAvailable")
        }
        center.addObserver(forName: UIApplication.protectedDataWillBecomeUnavailableNotification, object: nil, queue: .main) { [weak self] _ in
            self?.log("protectedDataWillBecomeUnavailable (locking)")
        }
        center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.flushPending(reason: "didBecomeActive")
        }
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [
                CBCentralManagerOptionRestoreIdentifierKey: Self.restoreIdentifier,
                CBCentralManagerOptionShowPowerAlertKey: false,
            ]
        )
    }

    // MARK: - Log

    static var logURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("doorbell.log")
    }

    static func readLog() -> String {
        (try? String(contentsOf: logURL, encoding: .utf8)) ?? "(empty)"
    }

    static func clearLog() {
        try? FileManager.default.removeItem(at: logURL)
    }

    private func log(_ message: String) {
        let line = "\(Self.timestamp()) \(message)\n"
        logger.log("\(message, privacy: .public)")
        let url = Self.logURL
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(Data(line.utf8))
            try? handle.close()
        } else {
            try? Data(line.utf8).write(to: url)
        }
    }

    private static func timestamp() -> String {
        let f = DateFormatter()
        f.dateFormat = "MM-dd HH:mm:ss.SSS"
        return f.string(from: Date())
    }

    private static func describe(_ state: UIApplication.State) -> String {
        switch state {
        case .active: "active"
        case .inactive: "inactive"
        case .background: "background"
        @unknown default: "unknown"
        }
    }

    // MARK: - Connection

    private func adopt(_ peripheral: CBPeripheral) {
        held[peripheral.identifier] = peripheral
        peripheral.delegate = self
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.peripheralKey)
    }

    private func connectAll() {
        guard central.state == .poweredOn else { return }
        var found = central.retrieveConnectedPeripherals(withServices: [BluetoothDiscovery.serviceUUID])
        log("retrieveConnected: \(found.count)")
        if found.isEmpty,
           let stored = UserDefaults.standard.string(forKey: Self.peripheralKey),
           let id = UUID(uuidString: stored) {
            found = central.retrievePeripherals(withIdentifiers: [id])
            log("retrieveByIdentifier \(stored): \(found.count)")
        }
        for peripheral in found {
            adopt(peripheral)
            connect(peripheral)
        }
    }

    private func connect(_ peripheral: CBPeripheral) {
        if peripheral.state == .connected {
            discover(peripheral)
        } else {
            log("connect \(peripheral.identifier) (state=\(peripheral.state.rawValue))")
            central.connect(peripheral) // pending connect: iOS re-dials when LE returns
        }
    }

    private func discover(_ peripheral: CBPeripheral) {
        if let service = peripheral.services?.first(where: { $0.uuid == BluetoothDiscovery.serviceUUID }) {
            subscribe(peripheral, service: service)
        } else {
            peripheral.discoverServices([BluetoothDiscovery.serviceUUID])
        }
    }

    private func subscribe(_ peripheral: CBPeripheral, service: CBService) {
        let wanted = [BluetoothDiscovery.characteristicUUID, Self.ringUUID]
        guard let characteristics = service.characteristics,
              wanted.allSatisfy({ uuid in characteristics.contains { $0.uuid == uuid } }) else {
            peripheral.discoverCharacteristics(wanted, for: service)
            return
        }
        for c in characteristics {
            if c.uuid == BluetoothDiscovery.characteristicUUID {
                peripheral.readValue(for: c)
            } else if c.uuid == Self.ringUUID, !c.isNotifying {
                log("setNotifyValue(true) on b7")
                peripheral.setNotifyValue(true, for: c)
            }
        }
    }

    // MARK: - Ring

    private func ring(_ value: Data?) {
        let body = value.flatMap { String(data: $0, encoding: .utf8) } ?? "<nil>"
        let state = Self.describe(UIApplication.shared.applicationState)
        log("RING \(body) app=\(state) bgTime=\(Self.backgroundTimeRemaining()) protected=\(UIApplication.shared.isProtectedDataAvailable) debugger=\(Self.debuggerAttached())")
        write("ring \(Self.timestamp())", label: "ring")

        guard !pulling else {
            ringDuringPull = true
            log("pull in flight, collapsing")
            return
        }
        pull()
    }

    private func pull() {
        pulling = true
        ringDuringPull = false
        var task: UIBackgroundTaskIdentifier = .invalid
        task = UIApplication.shared.beginBackgroundTask(withName: "doorbell-pull") { [weak self] in
            self?.log("background task EXPIRED")
            UIApplication.shared.endBackgroundTask(task)
            task = .invalid
        }
        let started = Date()
        log("pull start bgTask=\(task != .invalid) bgTime=\(Self.backgroundTimeRemaining())")

        Task { @MainActor in
            let result = await ShareSender.spikeFetchClipboardText()
            let ms = Int(Date().timeIntervalSince(started) * 1000)
            switch result {
            case .success(let text):
                self.log("pull OK \(ms)ms len=\(text.count)")
                if !self.write(text, label: "pull") {
                    self.pendingText = text
                    self.log("stashed pulled text for later")
                }
            case .failure(let error):
                self.log("pull FAILED \(ms)ms: \(error.localizedDescription)")
            }
            self.pulling = false
            if self.ringDuringPull {
                self.log("collapsed ring -> one more pull")
                self.pull()
            }
            if task != .invalid {
                UIApplication.shared.endBackgroundTask(task)
                task = .invalid
            }
        }
    }

    // Writes the pasteboard; returns whether changeCount moved (the write landed).
    @discardableResult
    private func write(_ text: String, label: String) -> Bool {
        dispatchPrecondition(condition: .onQueue(.main))
        let before = UIPasteboard.general.changeCount
        UIPasteboard.general.string = text
        let after = UIPasteboard.general.changeCount
        log("pasteboard \(label) write changeCount \(before) -> \(after) protected=\(UIApplication.shared.isProtectedDataAvailable)")
        return after != before
    }

    private func flushPending(reason: String) {
        log("\(reason) app=\(Self.describe(UIApplication.shared.applicationState)) pending=\(pendingText != nil)")
        guard let text = pendingText else { return }
        if write(text, label: "deferred") { pendingText = nil }
    }

    private static func debuggerAttached() -> Bool {
        var info = kinfo_proc()
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        var size = MemoryLayout<kinfo_proc>.stride
        sysctl(&mib, 4, &info, &size, nil, 0)
        return info.kp_proc.p_flag & P_TRACED != 0
    }

    private static func backgroundTimeRemaining() -> String {
        let t = UIApplication.shared.backgroundTimeRemaining
        return t > 1e6 ? "inf" : String(format: "%.1fs", t)
    }
}

// MARK: - CBCentralManagerDelegate

extension DoorbellSpike: CBCentralManagerDelegate {
    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] ?? []
        log("willRestoreState peripherals=\(peripherals.map { "\($0.identifier) state=\($0.state.rawValue)" })")
        peripherals.forEach(adopt)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        log("central state=\(central.state.rawValue) auth=\(CBManager.authorization.rawValue)")
        guard central.state == .poweredOn else { return }
        if held.isEmpty {
            connectAll()
        } else {
            held.values.forEach(connect)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("didConnect \(peripheral.identifier)")
        discover(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        log("didFailToConnect \(peripheral.identifier): \(error?.localizedDescription ?? "-")")
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("didDisconnect \(peripheral.identifier): \(error?.localizedDescription ?? "-"); re-dialling")
        central.connect(peripheral)
    }
}

// MARK: - CBPeripheralDelegate

extension DoorbellSpike: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == BluetoothDiscovery.serviceUUID }) else {
            log("service missing: \(error?.localizedDescription ?? "-")")
            return
        }
        subscribe(peripheral, service: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let uuids = service.characteristics?.map(\.uuid.uuidString) ?? []
        log("characteristics: \(uuids) error=\(error?.localizedDescription ?? "-")")
        if !(service.characteristics?.contains { $0.uuid == Self.ringUUID } ?? false) {
            log("b7 NOT FOUND (stale GATT cache? toggle Bluetooth off/on)")
        }
        for c in service.characteristics ?? [] {
            if c.uuid == BluetoothDiscovery.characteristicUUID {
                peripheral.readValue(for: c)
            } else if c.uuid == Self.ringUUID {
                log("setNotifyValue(true) on b7")
                peripheral.setNotifyValue(true, for: c)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        log("notify state b7 isNotifying=\(characteristic.isNotifying) error=\(error?.localizedDescription ?? "-")")
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == Self.ringUUID {
            ring(characteristic.value)
        } else if characteristic.uuid == BluetoothDiscovery.characteristicUUID {
            let info = characteristic.value.flatMap(LanInfo.decode)
            log("b6 name=\(info?.name ?? "-") fp=\(info?.fp.prefix(16) ?? "-") error=\(error?.localizedDescription ?? "-")")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        log("didModifyServices (GATT changed); rediscovering")
        peripheral.discoverServices([BluetoothDiscovery.serviceUUID])
    }
}

// MARK: - App delegate

final class DoorbellAppDelegate: NSObject, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        DoorbellSpike.shared.start(launchOptions: launchOptions)
        return true
    }
}
