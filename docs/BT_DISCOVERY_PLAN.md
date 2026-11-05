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

- [x] Linux: throwaway GATT app: `scripts/bt-discovery-spike.py` (logs every read with its `link`)
- [x] Linux: reachable over BR/EDR ATT? Moot: iOS central does not list the computer without LE, see Findings
- [x] iOS: throwaway probe written: `apple/Tether/Views/BTSpikeView.swift`, under Manual Connection, DEBUG only (compiles; not run on a phone yet)
- [x] Test: LE + BR/EDR up: read OK over LE, see Findings
- [x] Test: BR/EDR only (LE held down, tetherd stopped): `retrieve` returns 0 for the service and for 1800/1801. Needs LE
- [ ] Test: phone just back in range (moved to Phase 3 hardware run, against real tetherd)
- [x] Test: after `bluetoothd` restart: re-registers (Phase 1 build)
- [x] Go/no-go recorded in BLUETOOTH.md (dated entry): **go**, LE required. If no-go, the fallback is an advert with the
      service UUID plus `scanForPeripherals`.

### Running the spike

1. Linux: `scripts/bt-discovery-spike.py` (leave it running). For the bearer trace, also run
   `sudo btmon -w /tmp/spike.btsnoop`.
2. iPhone (DEBUG build from the Mac): Dashboard, then Manual Connection, then
   "Bluetooth discovery spike", then Run. Allow the Bluetooth prompt the first time.
3. Success means both of these happen:
   - the phone logs `read N bytes: {"v":1,...}`
   - the Linux script logs `ReadValue ... link=...`
4. If `retrieve(tether service): 0` but the control finds the computer, check the logged
   service list: it tells you whether iOS lacks the service or only had not cached it.

## Phase 1: tetherd (Linux)

- [x] `src/core/{include/tether,src}/bluetooth/lan_info.{hpp,cpp}`: `LanInfoService`, two-phase like
      `AncsAdvertisement` (export on the monitor thread, `RegisterApplication` off it)
- [x] Objects: `/org/tether/lan` (ObjectManager), `service0` (GattService1), `service0/char0`
      (GattCharacteristic1, `ReadValue` honours `offset`)
- [x] Pure `encode_lan_info(name, port, fp, addrs)` with filtering, ordering, and the 512-byte cap
- [x] Owned in `src/daemon/main.cpp`, re-registered on adapter change or `bluetoothd` restart,
      `AlreadyExists` counts as OK, silent no-op when there is no adapter or `GattManager1`
- [x] `test/test_lan_info.cpp`
- [x] BLUETOOTH.md section ("Discovery fallback")

## Phase 2: iOS (Mac)

- [x] `TetherFramework/BluetoothDiscovery.swift`: lazy `CBCentralManager`, `lookup() async -> [DiscoveredHost]`, 5s timeout
- [x] Pure `orderAddresses(addrs, phone:)`: same subnet first
- [x] `attemptAutoReconnect()`: Bluetooth lookup when Bonjour has no match, before the `lastEndpoint` fallback
- [x] "Find over Bluetooth" button in the manual-connect sheet for first-time setup
- [x] `.hostPort` hosts persist through `ShareSender.persistLastEndpoint` (line 566 covers it: `resolvedEndpoint` reads the path's remote endpoint for any endpoint type)
- [x] `NSBluetoothAlwaysUsageDescription` in `apple/Info.plist` and the pbxproj `INFOPLIST_KEY_`s, CoreBluetooth auto-linked by the import
- [x] `BluetoothDiscoveryTests.swift`: address ordering and JSON decode

## Phase 3: wrap-up

- [x] PROTOCOL.md "Bluetooth discovery" section (the contract above)
- [x] README: troubleshooting "The network blocks mDNS"
- [ ] Hardware run: avahi stopped, auto-connect over the same-subnet IP; BR/EDR-only bond;
      wifi + docker0 picks wifi; unbonded phone refused
- [x] Draft #287 reply (not posted; Linux box scratchpad)
- [ ] Delete this file

## Findings

<!-- YYYY-MM-DD (machine): what was tried, what was seen -->

- 2026-11-05 (Linux): BlueZ 5.87 `--experimental`, hci0. `RegisterApplication` worked from an
  unprivileged user, no polkit or dbus policy change needed. The value was 193 bytes with 2
  IPv4 and 1 IPv6 address. The iPhone was connected on both bearers at the time. Not checked:
  whether BlueZ exposes app services over BR/EDR ATT (`sdptool` is not installed); the btmon
  trace from the phone run will show it.

- 2026-11-05 (Mac): Phase 2 written against the wire contract. `swift test` and the iOS app build
  pass; not run on hardware yet, and it depends on the Phase 0 phone run (does
  `retrieveConnectedPeripherals` list the computer?). Auto-reconnect only does the Bluetooth
  lookup once Bluetooth permission is already granted, so updating users are never prompted out
  of nowhere; the "Find over Bluetooth" button is what asks.

- 2026-11-05 (Linux + iPhone 15 Pro): first spike run, both bearers up. iOS listed the computer and
  found the service. Advertised services: `180A, 184D, 1844, 1849, 184F, 185B, 03B80E5A-...
  (MIDI), BA7A69F7-...`. BlueZ logged one `ReadValue link=LE mtu=517 offset=0 len=193`, so the
  whole value fit in one read and no blob read was needed. The value carried `172.17.0.1/16`
  (docker0) next to the wifi `192.168.1.28/24`, so the same-subnet ordering is needed in practice.
  `retrieveConnectedPeripherals(withServices: [tether])` returned 1 straight away, with no
  fallback to the 1800/1801 control. Reported states: central 5 (poweredOn), authorization 3
  (allowedAlways). Four reads in total, all `link=LE`.

- 2026-11-05 (Linux + iPhone): BR/EDR-only test. `tetherd` was stopped and LE was held down with
  `Bearer.LE1.Disconnect`, while BR/EDR stayed connected. The phone logged
  `retrieve(tether service): 0` and `retrieve(1800/1801 control): 0`, and no read reached
  BlueZ. So the app only finds the computer while LE is up. Unknown whether that is a
  CoreBluetooth rule or only because no BR/EDR ATT channel (PSM 31) was open; there was no btmon
  capture.
  - What this means: Bluetooth discovery works for dual (LE) bonds, which `tetherd`'s bearer
    supervision normally keeps up. It does not work for Type=5 / BR/EDR-only bonds. The advert
    plus scan fallback would not help there, because `encrypt-read` needs LE keys. Document
    this as a known limit; it is not a blocker.
  - Also seen: with the app open, LE came back every 3-9s even with `tetherd` stopped, and it
    stopped after the app was force-quit. CoreBluetooth `connect()` stays pending and re-dials
    LE. `BluetoothDiscovery.swift` already cancels in both
    `finishRead` and `complete()`; the spike does not.

- 2026-11-05 (Linux): Phase 1 landed (uncommitted). `LanInfoService` exports `/org/tether/lan`,
  watches the `org.bluez` name, and re-registers whenever bluetoothd reappears or the preferred
  adapter changes. Every call runs async on the monitor thread. A debug `tetherd` logged
  `LAN info service registered on /org/bluez/hci0`, and `ctest` passed 639 of 639.
  - Differences from the plan: the service object is listed only in `GetManagedObjects`, which
    the spike showed is enough. There is no `--bt-status` line.
  - Hardware check: the phone's spike read returned the real fingerprint from the daemon.
    `systemctl restart bluetooth` showed one bug: the new bluetoothd took the `org.bluez` name
    about 20 ms before it exported hci0, so `RegisterApplication` failed with `UnknownObject`
    and was never retried. The fix is to retry on `InterfacesAdded` carrying `GattManager1`. With
    it, the second restart logged `adapter added` at 14:07:52.648 and `registered` at .655.

- 2026-11-05 (Linux + iPhone): Phase 3 run with avahi stopped. Auto-reconnect worked, but probably
  through iOS's Bonjour cache. "Find over Bluetooth" and the spike both failed with `The handle is
  invalid`.
  - btmon showed `ATT: Read Request Handle 0x009d` answered with `Error: Invalid Handle`, and no
    rediscovery beforehand. iOS read a handle it had cached before `tetherd`'s re-registration
    moved the service; auto-allocated handles follow registration order, and PipeWire's LE
    Audio and MIDI services register too.
  - Fix: the service `Handle` is pinned at 0xf000, so the value is at 0xf002 (BlueZ uses a nonzero
    `Handle` as given and does not write it back). After one Bluetooth off/on on the phone to
    refresh its cache, the spike and "Find over Bluetooth" both work.
  - Stability: after another `tetherd` restart, "Find over Bluetooth" still worked with no toggle.
  - `bluetoothd` restart with the pinned handle: re-registered 6 ms after hci0 came back. The first Find
    landed in the ~9 s before LE reconnected and found nothing; every Find after that worked.
