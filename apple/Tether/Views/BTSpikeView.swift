#if DEBUG
//
//  BTSpikeView.swift
//  Tether
//
//  Throwaway #287 Phase 0 probe: can the app read tetherd's LAN-info characteristic
//  over the system's existing Bluetooth bond? Delete with docs/BT_DISCOVERY_PLAN.md.
//

import CoreBluetooth
import SwiftUI

@Observable
final class BTSpike: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    static let service = CBUUID(string: "ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b5")
    static let characteristic = CBUUID(string: "ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b6")

    // Standard services every BlueZ host exposes. Used only to tell "iOS has not cached
    // our service" apart from "iOS does not list the computer at all".
    static let controls = [CBUUID(string: "1800"), CBUUID(string: "1801")]

    private(set) var log: [String] = []
    @ObservationIgnored private var central: CBCentralManager?
    @ObservationIgnored private var peripherals: [CBPeripheral] = []

    func run() {
        log.removeAll()
        peripherals.removeAll()
        guard let central else {
            // Creating the manager is what shows the permission prompt.
            central = CBCentralManager(delegate: self, queue: nil)
            return
        }
        retrieve(central)
    }

    private func say(_ line: String) {
        log.append(line)
        print("BTSpike: \(line)")
    }

    private func retrieve(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            say("central state \(central.state.rawValue), not poweredOn")
            return
        }
        var found = central.retrieveConnectedPeripherals(withServices: [Self.service])
        say("retrieve(tether service): \(found.count)")
        if found.isEmpty {
            found = central.retrieveConnectedPeripherals(withServices: Self.controls)
            say("retrieve(1800/1801 control): \(found.count)")
        }
        for p in found {
            say("peripheral \(p.name ?? "?") \(p.identifier) state \(p.state.rawValue)")
            p.delegate = self
            peripherals.append(p)
            central.connect(p)
        }
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        say("central state \(central.state.rawValue), authorization \(CBManager.authorization.rawValue)")
        retrieve(central)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        say("connected \(peripheral.name ?? "?")")
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        say("connect failed: \(error?.localizedDescription ?? "unknown")")
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { say("discoverServices error: \(error.localizedDescription)") }
        let services = peripheral.services ?? []
        say("services: \(services.map(\.uuid.uuidString).joined(separator: ", "))")
        guard let ours = services.first(where: { $0.uuid == Self.service }) else {
            say("tether service NOT present")
            return
        }
        peripheral.discoverCharacteristics([Self.characteristic], for: ours)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error { say("discoverCharacteristics error: \(error.localizedDescription)") }
        guard let chr = service.characteristics?.first(where: { $0.uuid == Self.characteristic }) else {
            say("characteristic NOT present")
            return
        }
        peripheral.readValue(for: chr)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            say("read error: \(error.localizedDescription)")
            return
        }
        let data = characteristic.value ?? Data()
        say("read \(data.count) bytes: \(String(data: data, encoding: .utf8) ?? "<not utf8>")")
    }
}

struct BTSpikeView: View {
    @State private var spike = BTSpike()

    var body: some View {
        List {
            Button("Run") { spike.run() }
            ForEach(Array(spike.log.enumerated()), id: \.offset) { _, line in
                Text(line)
                    .font(.caption.monospaced())
                    .textSelection(.enabled)
            }
        }
        .navigationTitle("BT discovery spike")
    }
}
#endif
