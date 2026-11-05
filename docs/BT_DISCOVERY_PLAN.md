# Bluetooth discovery fallback (#287): working plan

Temporary shared checklist for the Linux and Mac machines. Tick boxes, append to Findings. Delete
before merging to `main`.

## Goal

When mDNS is blocked, the app reads the daemon's addresses over the existing Bluetooth bond and
dials one over TCP. mDNS stays primary. Trust is unchanged: mTLS pinning decides, Bluetooth only
supplies an address.

## Wire contract

- Service UUID: `ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b5`
- Characteristic UUID: `ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b6`, flags `encrypt-read` only
  (bonded devices only).
- Value: UTF-8 JSON, at most 512 bytes, built fresh on every read:
  ```json
  {"v":1,"name":"host","port":5134,"fp":"<sha256 hex>","addrs":["192.168.1.20/24","2001:db8::5/64"]}
  ```
- `addrs` lists interfaces that are up. Loopback and IPv6 link-local are left out. IPv4 comes
  first, and entries are dropped from the tail until the value fits.
- Readers ignore unknown keys. A bump of `v` means an incompatible change.

## Phase 0: feasibility spike (go/no-go)

- [ ] Linux: throwaway GATT app via `bluetoothctl` `menu gatt` with the UUIDs above and a static value
- [ ] Linux: confirm the service is reachable over BR/EDR ATT as well as LE (`btmon`, `sdptool browse local`)
- [ ] iOS: throwaway button, `retrieveConnectedPeripherals(withServices:)`, then connect, discover, read
- [ ] Test: LE + BR/EDR up
- [ ] Test: BR/EDR-only bond (Type=5, #303)
- [ ] Test: phone just back in range
- [ ] Test: after `bluetoothd` restart
- [ ] Go/no-go recorded in BLUETOOTH.md (dated entry). If no-go, the fallback is an advert with the
      service UUID plus `scanForPeripherals`.

## Phase 1: tetherd (Linux)

- [ ] `src/core/{include/tether,src}/bluetooth/lan_info.{hpp,cpp}`: `LanInfoService`, two-phase like
      `AncsAdvertisement` (export on the monitor thread, `RegisterApplication` off it)
- [ ] Objects: `/org/tether/lan` (ObjectManager), `service0` (GattService1), `service0/char0`
      (GattCharacteristic1, `ReadValue` honours `offset`)
- [ ] Pure `encode_lan_info(name, port, fp, addrs)` with filtering, ordering, and the 512-byte cap
- [ ] Owned in `src/daemon/main.cpp`, re-registered on adapter change or `bluetoothd` restart,
      `AlreadyExists` counts as OK, silent no-op when there is no adapter or `GattManager1`
- [ ] `test/test_lan_info.cpp`
- [ ] BLUETOOTH.md section

## Phase 2: iOS (Mac)

- [ ] `TetherFramework/BluetoothDiscovery.swift`: lazy `CBCentralManager`, `lookup() async -> [DiscoveredHost]`, 5s timeout
- [ ] Pure `orderAddresses(addrs, phone:)`: same subnet first
- [ ] `attemptAutoReconnect()`: Bluetooth lookup when Bonjour has no match, before the `lastEndpoint` fallback
- [ ] "Find over Bluetooth" button in the manual-connect sheet for first-time setup
- [ ] `.hostPort` hosts persist through `ShareSender.persistLastEndpoint` (check whether line 566 already covers this)
- [ ] `NSBluetoothAlwaysUsageDescription` in `apple/Info.plist`, link CoreBluetooth
- [ ] `BluetoothDiscoveryTests.swift`: address ordering and JSON decode

## Phase 3: wrap-up

- [ ] PROTOCOL.md "Bluetooth discovery" section (the contract above)
- [ ] README line
- [ ] Hardware run: avahi stopped, auto-connect over the same-subnet IP; BR/EDR-only bond;
      wifi + docker0 picks wifi; unbonded phone refused
- [ ] Draft #287 reply (not posted)
- [ ] Delete this file

## Findings

<!-- YYYY-MM-DD (machine): what was tried, what was seen -->
