# #336 Background clipboard push to iPhone (BLE doorbell)

## Context

Issue #336 (fernando2393) asks for computer-to-iPhone clipboard sync while the app is in the
background. Today `handleScenePhase(.background)` calls `suspendForBackground()`
(`apple/Tether/ViewModels/TetherViewModel.swift:222,784`), which drops TLS and stops the server,
and `apple/Info.plist` declares no `UIBackgroundModes`. A copy made while the phone is locked is
lost. His branch (`fernando2393/tether@ef7794a`) adds two characteristics to the LAN-info GATT
service: `…b7` announce (notify) and `…b8` clipboard text (read, 512 bytes max). His follow-up
comment proposes a variant where BLE is only a doorbell and the data moves over mTLS Wi-Fi.

## Verdict on the analysis

**Direction: correct.** `bluetooth-central` plus state restoration is the only way to wake a
suspended iOS app without a push server. The project already has the pieces: the bonded LE
link, a tetherd GATT service at the pinned handle `0xf000`, and `BluetoothDiscovery`.

**The data-over-GATT version (b8) is the wrong design for Tether. Use the doorbell variant.**

- Security regression. b8 hands plaintext clipboard to *any* LE device bonded to the computer.
  It bypasses mTLS fingerprint pinning, trusted networks (#332), and any phone-side opt-in.
  Clipboards hold secrets, which was the whole point of #307.
- Two data paths: a 512-byte cut over BLE, then "TCP fetches the rest later". Text only, no
  images. The doorbell reuses `clipboard_get` over mTLS, which already returns images when the
  session negotiated `clipboard_image` (`src/core/src/net.cpp:2529`).
- `seq` resets on every daemon restart, so dedupe by seq drops the first copy after a restart.
  `size` leaks the clipboard length.
- The branch is based on an older main (no `clipboard_auto_share` gate at that call site). It also
  uses a raw `g_service` global and only hooks the Wayland text callback.

**Things the issue gets wrong or leaves out:**

- iOS does not keep the app alive. It suspends the app and wakes it for about 10 s per BLE event
  (longer with `beginBackgroundTask`). This is enough for one TLS pull.
- `BluetoothDiscovery` never holds a connection. It uses `retrieveConnectedPeripherals`, reads,
  then **cancels on purpose** (BLUETOOTH.md 2026-11-05: a connect that is never cancelled keeps
  re-dialling LE). A subscription needs a held connection, so this change alters LE bearer
  behaviour and needs a dated BLUETOOTH.md entry.
- It only works while LE is up. BR/EDR-only bonds get nothing (`retrieveConnectedPeripherals`
  returned 0 with only BR/EDR up, 2026-11-05).
- A force-quit from the app switcher disables relaunch. This is Apple's rule. A phone reboot may
  disable it too (needs verifying).
- Adding a characteristic to the service makes iOS's cached GATT table stale (the 2026-11-05
  stale-handle entry). The phone may not see `b7` until one Bluetooth off/on. This needs a check
  and an upgrade note.
- A background `UIPasteboard` write is untested. This is the go/no-go item.
- The phone-side opt-in matches the reason #338 was closed: the setting belongs on the phone, and
  the daemon keeps no memory. With the doorbell, the daemon only rings at copy time and the
  phone pulls the *current* clipboard, so nothing stale gets written.

**Files (comment) and the App Intent:** both are sound, but they are separate work. Files need a
daemon outbox, because today a send with no peer fails with `no_connected_mobile_client`
(`net.cpp:1828`). Do them after the clipboard doorbell is proven.

## Design (contract both sides build against)

- Characteristic `ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b7` in the existing service, flags
  `encrypt-read`, `notify`. Value `{"v":1,"seq":N}`. The phone ignores unknown keys, so `files`
  can be added later.
- A ring means "the computer's clipboard changed and may be shared". The phone pulls with
  `hello` (features `clipboard_image`) then `clipboard_get` over the normal mTLS session. No
  clipboard data goes over BLE.
- The daemon has no setting. The phone subscribes only when its setting is on.
- Both sides at once: the phone ignores rings while its TLS session is up (the foreground path
  already gets `clipboard_updated`). Rings that arrive during a pull collapse into one more pull.
  The daemon already suppresses the echo of selections it set itself
  (`src/core/src/wayland.cpp:284,338`), so a phone `clipboard_set` does not ring back.

## Phase 0: spike, go/no-go (both, about an hour, nothing merged)

- **Linux:** run a local build of fernando's branch (b7 already notifies, verified with nRF
  Connect), or the Phase 1 daemon if it lands first.
- **Mac:** a throwaway build with `bluetooth-central` and a restore id. It subscribes to b7, and
  on each notify writes `"ring <time>"` to `UIPasteboard.general`, then runs
  `ShareSender`'s connect + `clipboard_get`.
- **Checks:**
  - (a) app backgrounded, phone locked: the ring arrives and the paste works;
  - (b) the system terminated the app (memory pressure, or `kill` from Xcode): it relaunches via `willRestoreState`;
  - (c) after a phone reboot, before the app is opened;
  - (d) force-quit: expected to fail, record it;
  - (e) a TLS pull inside the wake window;
  - (f) the b7 cache problem after upgrade.
- **Go** if a, b, e and f-with-workaround pass. Record the result as a dated BLUETOOTH.md entry.

## Phase 1: Linux (this box)

- `src/core/include/tether/bluetooth/lan_info.hpp` and `src/core/src/bluetooth/lan_info.cpp`:
  - Add `UUID_TETHER_CLIPBOARD_RING` and the `char1` object, with `StartNotify`/`StopNotify`
    in the introspection XML and a `notifying` flag (BlueZ calls these once per characteristic,
    not per client).
  - Add `LanInfoService::ring_clipboard()`. It bumps `seq` and emits a `PropertiesChanged`
    `Value` on the monitor thread, using `invoke_sync` like the existing code.
  - `encode_clipboard_ring(seq)`.
  - Take the StartNotify and PropertiesChanged code from fernando's commit. Drop b8, `size`,
    `trunc` and the truncation code.
- `src/daemon/main.cpp`: reach the service through the existing `std::optional lan_info`
  (`main.cpp:795`), not a global. Ring at every site that pushes a clipboard to peers. Grep
  `clipboard_updated` and `broadcast_clipboard_image`:
  - the Wayland text callback (`main.cpp:264`, after the `clipboard_auto_share` gate, skipping
    empty text);
  - the image callback (`main.cpp:274`);
  - `clipboard_send` / `tether share` (`net.cpp:1781`), which must ring even when auto-share is off;
  - the peer `clipboard_set` (`net.cpp:2474`; harmless for the sender, useful for a second phone).

  `net.cpp` needs a ring hook (a `std::function` set from main) because it does not own `lan_info`.
- `test/test_lan_info.cpp`: test `encode_clipboard_ring`.
- Docs: a `docs/PROTOCOL.md` characteristic entry. In BLUETOOTH.md, rename "Discovery fallback"
  to cover both characteristics and add a dated entry (held connection, LE-only, cache toggle).
  Add a man page line if a user-visible note is needed. The README stays as is
 .
- Rebase note: credit fernando (Co-Authored-By) since the GATT notify plumbing is his.

## Phase 1: iOS (Mac)

- `apple/Info.plist`: add `UIBackgroundModes` = `[bluetooth-central]`. Update
  `NSBluetoothAlwaysUsageDescription` to mention clipboard.
- `SettingsView` + view model: a "Copy computer's clipboard in the background" switch, default
  off, stored in the app-group defaults. Turning it on raises the Bluetooth prompt if it has not
  been granted.
- `BluetoothDiscovery.swift` (or a sibling `BluetoothDoorbell` in TetherFramework that shares
  the UUIDs):
  - One long-lived `CBCentralManager` with `CBCentralManagerOptionRestoreIdentifierKey`. When the
    setting is on, create it at app launch (`TetherApp.init` / `initialize()`) so restoration
    finds it. Implement `centralManager(_:willRestoreState:)` and re-adopt the peripherals.
  - On `poweredOn`: `retrieveConnectedPeripherals([service])`, plus `retrievePeripherals` by a
    stored identifier. Then `connect` (held, never cancelled), discover, read `b6`, and check that
    `fp == lastConnectedFingerprint` and the host is known. Then `setNotifyValue(true)` on `b7`.
    The iOS pending connect re-dials when LE returns.
  - `lookup()` must not cancel the subscribed peripheral (`finishRead`/`complete` currently
    cancel every one).
  - Setting off: `setNotifyValue(false)` + `cancelPeripheralConnection`.
- Ring handler: if TLS is connected, ignore. Otherwise wrap one pull in `beginBackgroundTask`.
- `ShareSender.swift`: add a public `fetchClipboard() async -> ClipboardPull?`. It reuses
  `connectToLastKnownHost()`, sends `hello` with `clipboard_image`, then `clipboardGet`, and
  awaits `clipboard_content` (text, or a `file_*` image with `clipboard:"content"`). Write
  `UIPasteboard.general` unless the value equals the last one written (kept in defaults).
  Ponytail ceiling: pull goes to the last-connected computer only. A ring from a second
  computer needs a dial by b6's addresses.
- Tests: a `ShareSenderTests`-style unit test for ring decode and the "same as last written"
  skip.

## Later (not in this round)

- **Files over the doorbell:** a daemon outbox per target fingerprint (in memory, with a TTL),
  ring with `"files":true`, and flush on that peer's `hello`. The app saves to Documents and
  posts a `UNUserNotification` (the app requests no notification permission yet). Large files
  outrun the wake window and resume on the next wake.
- **App Intent "Send clipboard to computer"** (iOS only, independent): it takes a `String`
  parameter fed from Shortcuts' "Get Clipboard" (reading `UIPasteboard` in an intent prompts or
  fails), and its `perform` calls `ShareSender.send(.clipboard(text))`. Add an
  `AppShortcutsProvider` entry so it can go on Back Tap, the Action Button or Control Center.

## Split and handoff

1. Mac runs Phase 0 against fernando's branch built here (Linux: build it and hash the binary).
2. On go: Linux does Phase 1 Linux on `develop`. The Mac does Phase 1 iOS in parallel against
   the contract above.
3. Joint hardware run (below), then the BLUETOOTH.md entry, then the #336 reply. The reply
   thanks fernando, explains the switch to the doorbell, and asks him to test.

## Verification

- Linux: `ctest` passes. With nRF Connect, subscribe to b7, `wl-copy foo`, and a notify with
  seq+1 arrives. With `clipboard_auto_share` off there is no ring on copy, but `tether share`
  rings. Check with `busctl`/btmon that StartNotify/StopNotify toggle.
- Joint, on an iPhone with a dual bond:
  - setting on, lock the phone, `wl-copy` on Linux, unlock, and paste shows it;
  - an image copy works the same way;
  - the app is terminated by the system, then a copy still lands;
  - with the setting off, nothing is pulled;
  - with the phone on an untrusted network (#332), no pull happens and nothing crashes;
  - run `tether --bt-connection` before and after to confirm that holding the connection does
    not upset ANCS or bearer supervision;
  - two computers: only the last-connected one is pulled from.

## Phase 0 status: Linux side (2026-11-27)

- The spike daemon is running: fernando's `ef7794a` applied to `develop` `01927b4`, with
  spike-only log lines. It lives in a scratch worktree (not committed) and was started by hand.
  The running binary's sha256 starts `66aeb83b` (checked against `/proc/<pid>/exe`).
- It is registered on hci0: the adapter lists service `ba7a69f7-…b5`. The iPhone ("Zack 18 Pro")
  shows BR/EDR + LE + MAP up.
- Characteristics:
  - `…b6` LAN info, unchanged;
  - `…b7` announce: `encrypt-read` + `notify`, value `{"v":1,"seq":N,"size":S,"trunc":false}`;
  - `…b8` text: `encrypt-read`, up to 512 bytes.

  The Phase 1 doorbell drops b8, but the spike keeps it so the Mac can also test a read in the
  background.
- Checked: `wl-copy` bumps seq (`spike336: clipboard seq=2 listeners=0`). No phone has subscribed
  yet. The trust popup now fires only on b6 reads, not on b7 or b8.
- Log lines to watch, in `~/.local/state/tether/tetherd.log`:
  - `spike336: StartNotify on announce, listeners=1`: the phone subscribed;
  - `spike336: clipboard seq=N listeners=1`: a notify went out;
  - `spike336: ReadValue …char1/char2`: the phone read b7 or b8.
- Run (f) first: if iOS doesn't list b7 under the service, toggle Bluetooth on the phone once
  (the stale GATT cache, see BLUETOOTH.md 2026-11-05).

## Phase 0 results so far

**Run 1 (2026-11-27 19:19, phone locked, app attached to the Xcode debugger)**
- `…b7` showed up without a Bluetooth toggle. The app read `…b6` (fingerprint matched), then
  subscribed (`StartNotify` 19:18:38.304).
- The ring at 19:19:48.104 reached the app (`RING seq=3 size=24`). The pull over mTLS opened at
  .157 and returned `len=24` in 55 ms.
- **The pasteboard write failed while locked.** Reads gave `PBErrorDomain Code=10 "Pasteboard …
  is not available at this time"`, saves gave `Code=11 "… is not valid"`, and `changeCount`
  stayed at 0. Both the direct write and the post-pull write failed. The general pasteboard looks
  to be behind data protection while the device is locked.
- Not proven: wake from suspension. `bgTime=inf` means the debugger kept the app alive. Run again
  launched from the home screen, with no debugger attached, and read the logs in Console.app.

**Run 2 (phone unlocked, app freshly backgrounded):** the same `Code=10`/`Code=11` errors. iOS
(9 and later) blocks a background app from the general pasteboard, for reads and, on this
evidence, for writes too.

**Outcome:** a silent background clipboard write is not possible. The doorbell and the pull still
work. Options:
- the pulled text becomes a local notification, "Clipboard from <computer>: tap to copy", and
  the app writes it when it comes to the foreground;
- the doorbell is kept mainly for file sends, which need no pasteboard: save to Documents, then
  notify.
