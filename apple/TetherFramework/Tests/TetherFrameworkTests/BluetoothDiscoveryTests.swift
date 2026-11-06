//
//  BluetoothDiscoveryTests.swift
//  TetherFrameworkTests
//

import Foundation
import Testing
@testable import TetherFramework

struct BluetoothDiscoveryTests {
    @Test func decodesContractSample() {
        let json = #"{"v":1,"name":"host","port":5134,"fp":"abc123","addrs":["192.168.1.20/24","2001:db8::5/64"]}"#
        let info = LanInfo.decode(Data(json.utf8))
        #expect(info == LanInfo(v: 1, name: "host", port: 5134, fp: "abc123", addrs: ["192.168.1.20/24", "2001:db8::5/64"]))
    }

    @Test func ignoresUnknownKeys() {
        let json = #"{"v":1,"name":"host","port":5134,"fp":"abc","addrs":[],"future":{"x":1}}"#
        #expect(LanInfo.decode(Data(json.utf8))?.name == "host")
    }

    @Test func rejectsOtherVersionsAndMalformed() {
        let v2 = #"{"v":2,"name":"host","port":5134,"fp":"abc","addrs":[]}"#
        #expect(LanInfo.decode(Data(v2.utf8)) == nil)
        #expect(LanInfo.decode(Data("not json".utf8)) == nil)
        #expect(LanInfo.decode(Data(#"{"v":1,"name":"host"}"#.utf8)) == nil)
        #expect(LanInfo.decode(Data(#"{"v":1,"name":"h","port":0,"fp":"a","addrs":[]}"#.utf8)) == nil)
    }

    @Test func sameSubnetComesFirst() {
        let ordered = BluetoothDiscovery.orderAddresses(
            ["10.0.0.4/24", "192.168.1.20/24"],
            phone: ["192.168.1.77/24"]
        )
        #expect(ordered == ["192.168.1.20", "10.0.0.4"])
    }

    @Test func wifiBeatsDocker() {
        let ordered = BluetoothDiscovery.orderAddresses(
            ["172.17.0.1/16", "192.168.1.20/24"],
            phone: ["192.168.1.77/24", "100.64.3.2/32"]
        )
        #expect(ordered.first == "192.168.1.20")
    }

    @Test func matchesIPv6Prefix() {
        let ordered = BluetoothDiscovery.orderAddresses(
            ["10.0.0.4/24", "2001:db8:1::5/64"],
            phone: ["2001:db8:1::abcd/64"]
        )
        #expect(ordered == ["2001:db8:1::5", "10.0.0.4"])
    }

    @Test func keepsDaemonOrderWithoutMatch() {
        let ordered = BluetoothDiscovery.orderAddresses(
            ["10.0.0.4/24", "192.168.5.1/24", "2001:db8::5/64"],
            phone: ["172.20.10.2/28"]
        )
        #expect(ordered == ["10.0.0.4", "192.168.5.1", "2001:db8::5"])
    }

    @Test func dropsGarbage() {
        let ordered = BluetoothDiscovery.orderAddresses(
            ["nonsense", "192.168.1.20/33", "192.168.1.20/x", "", "192.168.1.21/24"],
            phone: []
        )
        #expect(ordered == ["192.168.1.21"])
    }

    @Test func phoneAddressesAreParseable() {
        for addr in BluetoothDiscovery.phoneAddresses() {
            #expect(!BluetoothDiscovery.orderAddresses([addr], phone: []).isEmpty, "\(addr)")
            #expect(!addr.hasPrefix("127."))
        }
    }
}
