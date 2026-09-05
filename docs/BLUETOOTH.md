# Bluetooth: iPhone messages and notifications

Tether accesses SMS/iMessage and other apps' notifications via Bluetooth because iOS apps are
not allowed to access these. It presents the Linux machine to iOS as a paired
Bluetooth accessory. No iOS-side code is involved.

| Feature | Mechanism | Transport | Linux role |
|---|---|---|---|
| SMS / iMessage, read + send | **MAP** (Message Access Profile) via BlueZ `obexd` | BR/EDR | OBEX client |
| Contacts, for sender names | **PBAP** (Phonebook Access Profile) via `obexd` | BR/EDR | OBEX client |
| Notifications from any app | **ANCS** (Apple Notification Center Service) | BLE / GATT | GATT central |
| Phone calls, place + answer | **HFP** (Hands-Free Profile) via BlueZ | BR/EDR | Hands-free unit |

The Tether iOS app is unrelated to these features and just keeps handling clipboard sync and file transfer over TCP + mTLS.

## Delivery modes

Not every machine can do all of it, so Tether resolves one of two modes from live capabilities.

- Full mode: MAP + PBAP + ANCS. Requires BR/EDR, LE, and advertising support, plus
  BlueZ >= 5.86 running with its experimental bearer API (org.bluez.Bearer.LE1).
- Compatibility mode: MAP + PBAP only. Messages and contacts work, notification mirroring does not (older BlueZ).

`tether --bt-status` reports which applies. `scripts/bt-probe.sh` is a
repo-only development probe that covers a few things the daemon does not check
(BlueZ version, `obex.service`, tooling); it is not installed by any package.
`scripts/bt-probe.sh --calls` is a separate mode for the call path, meant to be run
while a call is connected -- see "Calls".

## Setup requirements

Two of them need a system change, and both are reported with the exact command by:

```bash
tether --bt-setup
```

It prints only what is still missing and never applies anything: both steps
change how the machine behaves over Bluetooth outside Tether. The GTK app shows
the same list on the Devices page with a "Copy commands" button. The rest of this
section is what those commands do and why.

Class of Device must be A/V Hands-Free (major 4, minor 8). iOS only offers the
"Show Message Notifications" and "Sync Contacts" permissions to a device presenting this
class. `sudo btmgmt class 4 8` sets it for now.

**It does not survive a `bluetoothd` restart**. bluetoothd rewrites the
class from its own default every time it starts. Already-granted session survives that, but a phone that has not yet granted the permissions will refuse MAP and PBAP with an OBEX error that seems like a missing service record rather than
like a permissions problem.
A packaged install handles that with a unit it already put on disk:

```bash
sudo systemctl enable --now tether-btclass@hci0
```

`PartOf=bluetooth.service` re-runs it on every `bluetooth.service` restart. It writes the
class, reads it back, and retries for ten seconds.

**A portable build has no unit on disk to enable**, since neither the AppImage nor the Flatpak
can write a system directory. That command fails there with `Unit tether-btclass@hci0.service
does not exist`. The unit text is compiled into the binary instead, and `--bt-setup` prints the
form that applies to the running build: the AppImage prints `sudo "$APPIMAGE"
--install-btclass-unit` to write the unit out first, everything else prints it as a
here-document. Take the command from `--bt-setup` rather than from here.

**BlueZ needs the experimental bearer API** for ANCS, and it must be active
*before* pairing: a bond made without it has no LE half. `--bt-setup` prints the
drop-in command:

```bash
sudo mkdir -p /etc/systemd/system/bluetooth.service.d
printf '[Service]\nExecStart=\nExecStart=%s --experimental\n' \
  "$(ls /usr/lib/bluetooth/bluetoothd /usr/libexec/bluetooth/bluetoothd 2>/dev/null | head -1)" \
  | sudo tee /etc/systemd/system/bluetooth.service.d/experimental.conf
sudo systemctl daemon-reload && sudo systemctl restart bluetooth
```

`ExecStart` must name the `bluetoothd` binary and its path differs by distro
(Arch `/usr/lib`, Debian and Fedora `/usr/libexec`), so the command resolves it
in the user's own shell. A path chosen anywhere else — a drop-in shipped in the
package, baked at build time — is wrong on every distro but the builder's, and
a wrong `ExecStart` leaves `bluetooth.service` failing with `status=203/EXEC`.

Nothing is installed into `bluetooth.service.d` by the package: a drop-in there
takes effect the moment the package lands, and changing how `bluetoothd` runs
for the whole machine is the user's decision. The class unit ships but stays
disabled for the same reason, and `--install-btclass-unit` writes it without
enabling it on the builds that have no package to ship it.

Without it `bluetoothd` still registers `org.bluez.Bearer.LE1`, but as an empty
marker: no properties, no `Connect()`. So the interface being present is not
evidence the API is available, and code that reads it that way sees an LE bearer
that can never connect and a bond that never looks dual.

**`obexd` must be running** (user service `obex`) for MAP and PBAP. It is socket-activated under normal use.

## Pairing strategies

Two ways to make the bond, recorded in `auth_strategy` in `$XDG_CONFIG_HOME/tether/bluetooth.json`
(`~/.config/tether` by default).

**Connect-first** (`connect-first`, the default) calls `Device1.Connect()` on the unpaired
device. That is a *profile* connect, not an authentication request: it brings the ACL up, the
iPhone takes the central role, and iOS initiates the pairing. Making the phone the initiator is
what produces the cross-transport bond that carries ANCS, so this is what every recorded success
in this file used.

Its weakness is that it only works when BlueZ has some local BR/EDR profile the phone will accept
from an unbonded device. When it does not, bluetoothd logs one refused profile connect and tears
the link down before any security procedure runs:

```
src/profile.c:ext_connect() Hands-Free unit failed connect to <phone>: Connection refused (111)
```

**Explicit pair** (`explicit-pair`) calls `Device1.Pair()`, which requests authentication
directly and involves no profile, so neither refusal applies. This is what `bluetoothctl`'s own
`pair` command does.

**An explicit pair costs the LE half of the bond**, captured 2026-08-25 below. Messages and
contacts work on it; notifications never can. So it is a last resort, not an equal alternative.

Tether spends connect-first twice -- the iPhone's refusal of it is sometimes transient -- and
only then falls back to an explicit pair, and never falls back at all if the numeric comparison
was declined on this computer. A fallback bond is **not** remembered as the preferred strategy
while it comes back BR/EDR-only, so the next re-pair tries connect-first again from scratch;
latching it would put notifications permanently out of reach. The transaction that bonded is
reported as `auth_strategy_used` by `tether --bt-diagnostics`.

`tether --bt-pair <addr> --explicit-pair` forces the fallback for one transaction. Use it when
connect-first is refused on every attempt and messages and contacts are worth more than
notifications.

## Permissions on the phone

After pairing, the iPhone offers "Show Message Notifications" and "Sync Contacts" under
Settings -> Bluetooth -> (i) for the computer's entry. Off by default.

- The toggles can take minutes to appear after paring, and appear only while the ANCS 
  advertisement is actively broadcasting. Failed MAP connection attempts alone do not surface them.
- Closing and reopening the entry's detail page refreshes what iOS shows.
- A failed pairing can leave two records for the same computer, the toggles may appear
  under either one. Check both, and delete both before retrying a clean pairing.

Mirroring can be turned off without losing messages or contacts -- they ride BR/EDR OBEX, not the
LE bearer ANCS needs. Group threads are the exception: MAP gives them no conversation identifier,
so they are recognised only by correlating a Messages notification. Note also that the phone's
toggles surface only while the advert is broadcasting, so a permission that has to be re-granted
needs mirroring on to ask for it.

Without the relevant permission, MAP or PBAP is visible at the SDP level but rejects the
OBEX connection with `Forbidden` / `0x43`. That is a permissions state, not a pairing
failure (do not re-pair). A transport-level `Connection refused (111)` is different and
usually means another computer already owns the iPhone's single MAP session.

## Calls

Tether can place, answer and end calls on the iPhone. It implements no HFP.

BlueZ's own hands-free profile owns the RFCOMM link and the AT command layer, and
exports the result on the same object tree Tether already watches:

```
/org/bluez/hciN/dev_BDADDR/telephonyM        org.bluez.Telephony1
    Dial(s uri) -> object, SwapCalls, ReleaseAndAnswer, ReleaseAndSwap,
    HoldAndAnswer, HangupAll, CreateMultiparty, SendTones(s)
    UUID, State, Service, Signal, Roaming, BattChg, OperatorName,
    InbandRingtone, SupportedURISchemes

/org/bluez/hciN/dev_BDADDR/telephonyM/callK  org.bluez.Call1
    Answer, Hangup
    LineIdentification, IncomingLine, Name, Multiparty, State
```

Both are `[experimental]`, so they exist only under `bluetoothd --experimental` --
which Tether already requires for `org.bluez.Bearer.LE1`. They appear only while the
phone has Hands-Free connected, and their absence is a normal state, not a fault.
Documented in `man org.bluez.Telephony` and `man org.bluez.Call`, with a
`bluetoothctl` submenu behind `menu telephony`.

`BluezMonitor` already subscribes to `InterfacesAdded`/`InterfacesRemoved` and to
`PropertiesChanged` across `org.bluez`, so the telephony objects arrive in the
snapshot with no new bus, thread or subscription. `TelephonyClient` holds no state at
all: it reads the snapshot and issues method calls on the monitor's connection.

### The audio stays on the phone

Control only, by construction. Ringing, caller ID, answering, hanging up and dialling
all work from the desktop; the call itself plays on the iPhone.

BlueZ's hands-free profile never opens the voice link. HFP is two connections -- RFCOMM
for the AT commands, SCO for the audio -- and BlueZ only uses the first. Three things
say so:

- `bluetoothd` contains no `AT+BCC` and no `+BCS`. Those are the Codec Connection
  commands, the only way a hands-free unit asks the phone to open the voice link. The
  rest of the AT vocabulary is complete: `BRSF`, `CIND`, `CMER`, `CLIP`, `CCWA`, `COPS`,
  `CLCC`, `CHLD`, `CHUP`, `BLDN`, `+CIEV`.
- `profiles/audio/transport.c`, which implements `org.bluez.MediaTransport1`, links with
  `a2dp.c`, `bap.c` and `asha.c`. Its states are `idle/pending/broadcasting/active` with
  `Links` for LE Audio BIGs. There is no SCO transport type in it.
- Measured **during an active call**, where a lazily created transport would have shown
  up: BlueZ exports no `/fd#` object and PipeWire creates no `bluez_card`.
  `./scripts/bt-probe.sh --calls` checks all of this in one command.

So the consequence is that **no WirePlumber configuration is needed or wanted**. The
`hfp_hf` role stays off, the machine never becomes an audio destination for the phone,
and "Keeping the phone's audio on the phone" below continues to apply unchanged.

`org.bluez.Telephony1` is also where desktop call audio arrives on its own eventually.
It is not an HFP-specific interface: `profiles/audio/telephony.c` is a shared layer
sitting alongside `profiles/audio/ccp.c` (LE Audio Call Control), and the `UUID`
property exists because more than one profile can provide it -- an iPhone over HFP
reports `0000111f`, an LE Audio phone would report a TBS UUID through the same methods
and the same `Call1` objects. LE Audio call audio is carried by an ordinary BAP
`MediaTransport1`, which every audio server already consumes. Tether would need no
change for either that or a future SCO export.

### What was tried instead

PipeWire's `bluez5` plugin implements the same profile in the HF role and publishes
`org.pipewire.Telephony`, and it *can* carry the audio (`AudioGatewayTransport1.Activate()`,
with `bluez5.telephony.default-reject-sco` to keep it on the phone until asked).

It cannot be used at the same time. Both register for UUID `0000111e`, and BlueZ's
built-in profile wins: `Device1.Connect()` routes into it and fails, and PipeWire's
profile never gets the link.

```
profiles/audio/hfp-hf.c:hfp_connect() unable to start connection
btd_service_connect() hfp profile connect failed for <phone>: Input/output error
```

Dropping `hfp_hf` from `bluez5.roles` makes BlueZ's profile connect immediately and
export `telephony0`. Using PipeWire's instead would mean `bluetoothd --noplugin=hfp`,
a system-wide change to every Bluetooth device on the machine, in exchange for desktop
call audio. That trade was not taken. The reasons are about maintenance across a large
and varied install base, not capability:

- **Two prerequisites, both moving.** PipeWire >= 1.4 for the telephony API, *and*
  `--noplugin=hfp` -- but only on BlueZ >= 5.87, where the built-in profile exists.
  The required system configuration therefore differs per distro, and appears under
  existing users when their distro bumps BlueZ. Working calls would break on an
  unrelated upgrade with no change in Tether.
- **A CI target cannot run it.** `.github/workflows/ubuntu.yml` builds against Ubuntu
  24.04, an LTS supported to 2029, whose PipeWire predates the telephony API entirely.
- **Not every machine has PipeWire.** PulseAudio hosts, servers, containers.
- **It would bind Tether to one audio server permanently**, where
  `org.bluez.Telephony1` is already the profile-agnostic layer described above.

BlueZ's route has one prerequisite, `bluetoothd --experimental`, which Tether already
requires for `org.bluez.Bearer.LE1`, already detects, and already prints the fix for.

Users who want the audio on the desktop can still have it, at the cost of Tether's call
control -- see below.

### Getting the call audio onto the desktop instead

Possible, and it costs more than it first appears. Walked end to end on 2026-09-04, so
what follows is measured rather than reasoned.

You give up two things:

- **Tether's call control.** Handing the profile to PipeWire means BlueZ no longer owns
  it, exports no `org.bluez.Telephony1`, and the Calls page goes empty.
- **The phone's audio staying on the phone.** `a2dp_sink` turns out to be mandatory
  here, so music and system sounds move to the desktop as well. Everything
  "Keeping the phone's audio on the phone" below is written to avoid, you are opting
  back into. This is the part that surprises people, so it is stated first.

Tether has no code on this path and does not test it in CI.

All three settings are required. Any one missing and nothing works at all.

1. Stop BlueZ's built-in profile claiming UUID `0000111e`. Only needed on BlueZ >= 5.87;
   older builds have no built-in to disable.

   ```bash
   sudo mkdir -p /etc/systemd/system/bluetooth.service.d
   printf '[Service]\nExecStart=\nExecStart=%s --experimental --noplugin=hfp\n' \
     "$(ls /usr/lib/bluetooth/bluetoothd /usr/libexec/bluetooth/bluetoothd 2>/dev/null | head -1)" \
     | sudo tee /etc/systemd/system/bluetooth.service.d/no-hfp.conf
   sudo systemctl daemon-reload && sudo systemctl restart bluetooth
   ```

   Keep `--experimental`: dropping it takes notification mirroring with it.

2. Give PipeWire the roles and turn its telephony service on, in
   `~/.config/wireplumber/wireplumber.conf.d/51-no-phone-audio.conf`:

   ```
   monitor.bluez.properties = {
     bluez5.roles = [ a2dp_source a2dp_sink hfp_ag hfp_hf bap_source ]
     bluez5.telephony-dbus-service = true
     bluez5.telephony.default-reject-sco = true
   }
   ```

   Then `systemctl --user restart wireplumber` and reconnect the phone. On
   WirePlumber 0.4 the same setting goes in `bluetooth.lua.d` as
   `bluez_monitor.properties["bluez5.roles"]`, as described below.

   `a2dp_sink` is not optional, however much you want it to be. Without it iOS does not
   treat the machine as an audio destination, does not list it in the Control Center
   audio picker, and never connects hands-free to it -- PipeWire creates no
   `bluez_card` at all. Adding it makes the card appear immediately.

   `bluez5.telephony-dbus-service` is not optional either. `org.pipewire.Telephony`
   being an owned bus name does not mean the service is enabled, and without this
   setting no gateway object is ever registered.

3. **Let the phone initiate.** The desktop cannot bring the link up. `Device1.Connect()`
   ignores PipeWire's external profile; `ConnectProfile("0000111e")` fails with
   `No more profiles to connect to`, because the iPhone advertises `0000111f` and BlueZ
   matches against the remote's UUID list; `ConnectProfile("0000111f")` returns success
   and does nothing. Select the machine on the phone -- Control Center's audio route, or
   Settings > Bluetooth -- and the hands-free link comes up on its own.

Success looks like this in WirePlumber's log
(`WIREPLUMBER_DEBUG='*bluez*:5' wireplumber`):

```
spa.bluez5.native   register_profile: Registering Profile /Profile/HFPHF 0000111e-...
spa.bluez5.native   profile_new_connection: NewConnection ... fd=59, profile /Profile/HFPHF
spa.bluez5.native   rfcomm_send_next_cmd: RFCOMM >> AT+BRSF=695
spa.bluez5.telepho  telephony_ag_register: registered AudioGateway: /org/pipewire/Telephony/ag1
```

and `busctl --user tree org.pipewire.Telephony` then shows the gateway. Call control
lives on `org.pipewire.Telephony`, mirrored as `org.ofono.VoiceCallManager` and
`org.ofono.VoiceCall`, so any oFono-compatible dialer can drive it while PipeWire
carries the audio. `AudioGatewayTransport1.Activate()` pulls the audio to the desktop.
PipeWire older than 1.4 publishes no telephony API at all, leaving audio but no call
control.

Watch for `Not activating device bluez_card.<ADDR>` in WirePlumber's log. That is
`create-device.lua` declining a card whose `api.bluez5.connection` is not yet
`connected`, and it means the hands-free service level connection has not finished --
the AT handshake above is still in flight, or was interrupted.

To undo it: delete `/etc/systemd/system/bluetooth.service.d/no-hfp.conf`, restore the
`bluez5.roles` line to whatever it was, `sudo systemctl daemon-reload && sudo systemctl
restart bluetooth`, `systemctl --user restart wireplumber`, and reconnect the phone.
`./scripts/bt-probe.sh --calls` should show `telephony0` again.

### Turning it on

`tether --bt-calls-enable on`, or the Calls page in the GTK app. Off by default.

It also widens the pairing agent's service whitelist to the hands-free and headset
UUIDs (`agent.cpp` `is_authorized_service`), which the iPhone authorizes against during
pairing. With calls off, those stay refused. A bond made before calls were enabled
keeps working; only a fresh pairing goes through the agent.

### Commands

```bash
tether --bt-calls                    # what is ringing, dialing or connected
tether --bt-call +15555550123        # dial
tether --bt-answer                   # answer the ringing call
tether --bt-hangup                   # end every call
tether --bt-calls-enable on|off
```

`tether --bt-connection` reports the gateway alongside the other profiles, with the
carrier, signal and phone battery HFP supplies.

Dial strings are normalized in the daemon: a leading `+` and digits survive, spaces,
dashes, dots and parentheses are dropped, and anything else is refused rather than
handed to the cellular network. BlueZ itself accepts `[0-9+*#,ABCD]{1,80}`, so this is
the narrower check; it rules out the `*`/`#` supplementary-service codes deliberately.

### What it does not do

- No call audio on the desktop, per above. `./scripts/bt-probe.sh --calls`, run during a
  call, reports which of the two states this machine is in.
- No call history. A missed call still arrives as an ANCS `MissedCall` notification.
- `HangupActive` and `HangupHeld` are documented but absent from BlueZ 5.87, so only
  `HangupAll` is offered.
- Caller ID is whatever HFP carries. When the phone sends no name, Tether fills it from
  the PBAP address book by number. A network that refuses caller ID sends the literal
  `withheld`, which is shown as unknown and never offered to redial.

## Conflicts

The iPhone serves one MAP session at a time. Any other program on any machine holding it will block Tether.

## Known limits

These are properties of what iOS exposes:

- MAP reports both SMS and iMessage as `Type: sms-gsm`. The transport a message used is not knowable.
- The iPhone's MAP sent folder is empty and sending produces no useful outgoing event, so sent history cannot be recovered from the phone. Tether records its own sends.
- ANCS supports positive/negative notification actions only. There is no free-text reply over ANCS, replies go through MAP.
- No attachments, reactions, typing indicators, or read receipts.
- MAP gives no conversation identifier and no participant list for group messages, so group support is a guess and is conservative by default.

## Troubleshooting

Start with the read-only checks. `tether --bt-setup` says what system setup is
still missing, `tether --bt-status` says what the hardware and the stack can do,
and `tether --bt-connection` says what is actually up right now. From a source
checkout, `./scripts/bt-probe.sh` adds BlueZ-version, tooling, and `obexd`
checks the daemon does not make.

| Symptom | Cause | What to do |
|---|---|---|
| `systemctl enable --now tether-btclass@hci0` hangs in `activating (start)` forever | `btmgmt` epolls its stdin before running the command, and epoll rejects the `/dev/null` systemd hands it, so it waits having done nothing | Update the unit -- it pipes into `btmgmt` now. On an older build, `sudo btmgmt class 4 8` by hand sets the class until `bluetoothd` restarts, see 2026-09-01 below |
| `systemctl enable --now tether-btclass@hci0` says `Unit tether-btclass@hci0.service does not exist` | A portable build (AppImage, Flatpak) installed no unit -- only the distro packages do | `tether --bt-setup` and run the command it prints, which writes the unit first, see 2026-09-05 below |
| Messages and contacts worked, then stopped, and the error mentions a service record | `bluetoothd` restarted and reset the Class of Device | `sudo systemctl enable --now tether-btclass@hci0`, then re-pair if the phone dropped the bond |
| The phone never offers notifications / Sync Contacts | The class is wrong, or the ANCS advertisement is not running | Check for `class=ok` in `tether --bt-status`, can take minutes |
| MAP or PBAP reports `forbidden` | The matching toggle on the phone is off | Turn it on. This is not a pairing failure |
| Pairing never starts, and the only log line is a profile connect refused with `Connection refused (111)` | `Device1.Connect()` induces pairing only as a side effect of a profile connect, and this phone refuses that profile from an unbonded device | Nothing. Tether retries the transaction as an explicit `Device1.Pair()` on its own. To go straight there, `tether --bt-pair <addr> --explicit-pair` |
| The phone shows a pairing code, then "Pairing Unsuccessful" a moment later, and the daemon reports the transaction failed about 90s after `confirm` | `tetherd` has no display, so the confirmation dialog could not be shown, and an unshowable dialog used to count as a refusal | Fixed. The comparison now goes to whichever client started the pairing -- the CLI prompts on the terminal, the GTK app opens its own dialog. On an older build, start `tetherd` from a graphical session so it inherits `DISPLAY` or `WAYLAND_DISPLAY` |
| Pairing fails a few hundred ms after `confirm`, and `tetherd.log` says `tether-dialog: error while loading shared libraries: libgtk-layer-shell.so.0` | `tether-dialog` is linked against `gtk-layer-shell`, which the package did not depend on, so it died in the dynamic loader with exit 127 -- read as the user refusing | Install `gtk-layer-shell`. Fixed in the package dependencies, and a dialog that cannot run now routes the comparison to the client that started the pairing instead of declining it -- see 2026-08-29 below |
| MAP reports `busy`, or the transport says `Connection refused (111)` on an already-paired phone | Another computer holds the iPhone's single MAP session | Stop the other client |
| Pairing fails with `br-connection-key-missing` | A stale bond on one side, or the adapter is not `Pairable` | Delete the computer's entry on the phone (Forget This Device) and `tether --bt-unpair <addr>` locally, then pair again |
| The phone shows two entries for this computer | A failed pairing left both a Classic and an LE record | Delete both on the phone before retrying |
| LE never connects and the log repeats `org.bluez.Error.InProgress` | BlueZ is holding an auto-connect registration that never completed | `sudo systemctl restart bluetooth` -- nothing short of that clears it, see 2026-08-19 below. With `tether-btclass@hci0` enabled the class survives the restart |
| The log says `could not re-arm the ANCS solicitation` | BlueZ refused to register the advertisement, so nothing is on air for the iPhone to answer | `sudo systemctl restart bluetooth`. Nothing else brings it back, and the LE link cannot form without it |
| Pairing warns `RegisterAdvertisement ... doesn't exist` on `org.bluez.LEAdvertisingManager1` | BlueZ exported no advertising manager on this adapter at all, because the controller reports no LE advertising support. Distinct from the row above, where the interface exists and the call is refused | Restarting `bluetooth.service` changes nothing. Confirm with `./scripts/bt-probe.sh`; the only route to ANCS is a controller that can advertise, selected with `tether --bt-adapter <hciN>` -- see 2026-09-03 below |
| LE never connects and the log repeats `le-connection-abort-by-local` | Something on this side is cancelling the connection. Tether's own cause was a `PreferredBearer` write racing the async connect, fixed; anything else writing that property during a connect will do the same | Check no other Bluetooth tool is driving the same device. The phone is not the cause: `abort-by-local` means the local host cancelled |
| Messages stopped after turning notification mirroring off | Fixed. The toggle used to restart supervision, which abandoned the MAP and PBAP sessions at obexd instead of removing them, and the iPhone serves one MAP session at a time | Nothing. Mirroring is switched in place now, and a dropped profile supervisor releases its sessions. On an older build, restart `tetherd` |
| Notifications stopped and never came back, while messages and contacts kept working | Fixed. The bearer supervisor used to stop retrying LE after six attempts, and only a Classic drop or a daemon restart re-armed it | Nothing. The solicitation is kept on air whenever LE is down, which is what the iPhone answers -- see 2026-08-22 |
| `tether --bt-status` reports `Bond: BR/EDR only` | The bond was made without cross-transport key derivation, so it has no LE half and can never carry ANCS | Forget this computer on the iPhone and pair again -- it can take more than one attempt, the derivation is flaky on identical inputs (2026-08-25). Check `secure-connections` in the same output first: re-pairing cannot help while it is off |
| Walked back into range and nothing reconnected for minutes | Fixed. The ANCS advert was gated on the Classic link, and the Classic backoff had no event that ended the absence | Nothing. The advert stays on air whenever LE is down, and an LE link coming up clears the Classic backoff -- see 2026-08-22 |
| Startup logs `StartNotify not ready yet (InProgress)` for up to a minute | GATT discovery is still running on the new LE link | Nothing. It subscribes on its own. Only treat it as the 2026-08-19 hang if the LE link never comes up |
| `tether --bt-connection` reports LE and messages up but `Notifications: no`, for hours | Fixed. The LE link was opened by the dial and carries no ANCS. A connected link used to take the solicitation off air, so the phone was never asked for the service | Nothing. The advert goes back on air over a link that has stayed up without ANCS -- see 2026-08-23. To clear it by hand on an older build, `tether --bt-solicit`; do not re-pair, and do not cycle the phone's Bluetooth |
| LE never comes up on a `BR/EDR + LE` bond, the advert is on air, and cycling the phone's Bluetooth changes nothing | The bond is pinned to `PreferredBearer=bredr`, so the inbound LE link the iPhone opens is never accepted | Fixed for new bonds, which are handed back to `le` after pairing. An older bond stays pinned: re-pair it, or set the property by hand with `busctl set-property org.bluez /org/bluez/hci0/dev_<ADDR> org.bluez.Device1 PreferredBearer s le` -- see 2026-08-25 below |
| The status says the iPhone is not answering on LE, and its permission is on | The phone's Bluetooth stack is wedged, which the granted permission does not prevent | Turn Bluetooth off and back on **on the iPhone**. Re-pairing and re-toggling the permission do not clear this |
| The status says this computer is not putting the notification request on air | The adapter reports LE advertising support and BlueZ is holding no advertising instance for it, so the iPhone is never asked for the service | Nothing on the iPhone, and re-pairing will not help. Check `controller` in `tether --bt-diagnostics` and try another with `tether --bt-adapter <hciN>` -- see 2026-09-04 below |
| Everything connects but `ancs_ready` stays false | Compatibility mode, or iOS has not authorized notification content yet | Check `Mode:` in `tether --bt-status`. In full mode the daemon retries, the first request returns `NotPermitted` until the prompt on the phone is approved |
| A group conversation cannot be replied to | Working as designed until the route is unambiguous | The thread's `reply_reason` says which condition failed |
| The iPhone never offers the "Show Notifications" toggle, and messages and contacts work | Fixed. A BR/EDR-only bond used to latch notification mirroring off in the config, which takes the ANCS solicitation off air -- so the phone is never asked for the service | Nothing. On an older build, `tether --bt-ancs on` then `tether --bt-solicit`; `tether --bt-status` now shows mirroring under `Notifications:` -- see 2026-09-01 |
| Pairing bonds but the LE half never derives, on a machine with a USB dongle plugged in | Tether used the first powered controller, which is the dongle, not the built-in one | `tether --bt-status` marks the controller in use; `tether --bt-adapter <hciN>` picks another -- see 2026-09-01 |
| The link reads down forever with `br-connection-unknown`, while messages, contacts and notifications all work | This computer offers the iPhone no BR/EDR profile to connect to, and BlueZ only reports a link up while some local profile is connected | Nothing. Tether no longer waits on that link -- see 2026-08-23 below. Call support does not change this: BlueZ's hands-free profile is not one of the local profiles BlueZ counts |
| The iPhone's audio moves to the computer when Tether connects | The machine advertises itself as a Bluetooth speaker/headset, and iOS routes to it. Not caused by Tether beyond bringing the link up | See "Keeping the phone's audio on the phone" below |
| `tether --bt-calls` reports call control off | The iPhone has not connected Hands-Free, or `bluetoothd` is running without `--experimental` | The daemon's reason line names which. Confirm with `busctl --system tree org.bluez \| grep telephony` |
| Calls work but the audio is on the iPhone | Working as designed. BlueZ signals the call and never opens the voice link, so there is nothing to route here | Nothing. `./scripts/bt-probe.sh --calls` during a call shows the evidence; "Getting the call audio onto the desktop instead" is the trade if you want it |
| The Calls page is empty after configuring PipeWire for call audio | Expected. PipeWire owns the hands-free profile now, so BlueZ exports no `telephony0` for Tether to drive | Pick one: the revert steps under "Getting the call audio onto the desktop instead", or keep PipeWire and use an oFono-compatible dialer |
| Configured PipeWire for call audio and the machine is not in the iPhone's audio picker | `a2dp_sink` is missing from `bluez5.roles`. iOS only speaks hands-free to a machine it considers an audio destination | Add `a2dp_sink`, and accept that the phone's music comes here too -- see "Getting the call audio onto the desktop instead" |
| `hfp_connect() unable to start connection` in the bluetoothd log | PipeWire's `hfp_hf` role and BlueZ's built-in profile are both claiming UUID `0000111e` | Remove `hfp_hf` from `bluez5.roles` -- see "Calls" and 2026-09-04 |

### Keeping the phone's audio on the phone

Unaffected by call support: calls run over BlueZ's own profile and never make this
machine an audio destination. This still applies exactly as written.

Once the Classic link is up, the iPhone's calls, music, and system sounds play on the
computer instead of the phone. PipeWire registers A2DP sink and HFP audio-gateway
endpoints for every adapter, so the machine advertises itself as a speaker and headset,
and iOS routes to a bonded device that offers one. Tether only brings the link up; the
routing decision is the phone's. Every desktop with Bluetooth audio behaves this way.

To confirm it, with the phone connected:

```bash
pactl list cards
```

The phone appears as `bluez_card.<ADDR>` with `Active Profile: audio-gateway`.

The fix is to stop advertising the roles a phone connects to, while keeping the ones
headphones use. On WirePlumber 0.5 and later, write
`~/.config/wireplumber/wireplumber.conf.d/51-no-phone-audio.conf`:

```
monitor.bluez.properties = {
  bluez5.roles = [ a2dp_source hfp_ag bap_source ]
}
```

Then `systemctl --user restart wireplumber` and reconnect the phone. Role names are
from this machine's perspective, not the remote device's: `a2dp_source` and `hfp_ag`
are the roles that drive headphones, `a2dp_sink` and `hfp_hf` are the roles that make
the machine a destination for a phone. Dropping the second pair leaves the iPhone
nothing to route to, so its audio stays local, while headphones keep both A2DP
playback and the HFP microphone. `bap_source` keeps LE Audio playback; drop it too if
nothing here uses LE Audio. On WirePlumber 0.4 the same setting goes in
`~/.config/wireplumber/bluetooth.lua.d/51-no-phone-audio.lua` as
`bluez_monitor.properties["bluez5.roles"]`.

If you later want call audio on the desktop, be aware this setting is the thing in the
way: `a2dp_sink` is what makes iOS willing to speak hands-free to this machine at all.
See "Getting the call audio onto the desktop instead" -- you cannot keep the music here
and move the calls.

Two things that look like fixes and are not:

- `pactl set-card-profile bluez_card.<ADDR> off` stops the computer playing the audio,
  and WirePlumber remembers it in `~/.local/state/wireplumber/default-profile`, but the
  iPhone still believes it is routed to the computer. The audio goes nowhere and the
  phone is silent.
- `device.disabled = true` in a `monitor.bluez.rules` entry does nothing. Only the
  alsa, v4l2, and libcamera monitors honour that property.

One side effect is worth knowing. `a2dp_sink` was very likely the only BR/EDR profile
this machine could connect to an iPhone, so dropping it leaves `Device1.Connect` with
nothing to connect and BlueZ reporting the device disconnected however healthy the
radio is. That is expected, and it no longer blocks anything: messages, contacts, and
notifications do not run over that link. A machine with no audio stack at all -- a
server, a container -- is in the same position from the start.

### Reporting a problem

```bash
tether --bt-diagnostics
```

Prints the delivery mode, auth strategy, Bluetooth settings, current connection state, and timeline of recent link and pairing transitions.
It names the controller too: `controller` under `status.adapters` is the chip, from
sysfs. The `modalias` beside it is BlueZ's own device id, not the hardware.

It is redacted for pasting into an issue. Bluetooth addresses, phone numbers, email
addresses, and home and runtime directories become numbered placeholders.
Messages, contact names, and notification content are dropped, and message and notification events never enter the timeline at all. Read it before
you post it anyway.

## Recorded results

Per the maintenance rule below, every entry records phone model, iOS version, BlueZ
version, and controller, and distinguishes what was captured from what was inferred.

### 2026-07-11 — MediaTek MT7925, BlueZ 5.87, iPhone 15 Pro full mode

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| Kernel | 7.1.8-arch1-3 (Arch Linux) |
| BlueZ | 5.87, running with `-E` |
| Adapter roles | central + peripheral; 15 advertising instances |
| Adapter class | `0x7c0408` — A/V Hands-Free |
| Phone | iPhone 15 Pro, iOS _(version not recorded - fill in)_ |

Captured observations against the bonded phone:

- Dual bond confirmed. `Device1` reports `Paired`, `Bonded`, `Connected`, and `Trusted` all true, and `org.bluez.Bearer.LE1` independently reports `Paired`, `Bonded`, and `Connected` all true. One bond covers BR/EDR and LE.
- ANCS is live. Service `7905f431-b5ce-4e99-a40f-4b1e122d00d0` is in the GATT tree along with all three characteristics: Notification Source
  `9fbf120d-6301-42d9-8c58-25e699a21dbd`, Control Point
  `69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9`, and Data Source
  `22eac6e9-24d6-4bb5-be44-b36ace7c7bfb`.
- MAP and PBAP are advertised in the device's profile UUID list: `0x1132` (Message Access Server), `0x1133` (Message Notification Server), and `0x112f` (Phonebook Access Server).
- Apple Media Service `89d3502b-0f36-433a-8ef4-c502ad55f8dc` is also present.

### 2026-07-13 — MAP/PBAP refused while ANCS worked

Captured with the bond fully connected (`Connected` and `ServicesResolved` true, `Bearer.LE1` connected) and the phone unlocked and attended:

- Opening either OBEX session failed with `org.bluez.obex.Error.Failed: Unable to find service record`.
- The same call made by hand with `busctl --user call org.bluez.obex ... CreateSession` failed identically, so this is obexd's SDP lookup, not a Tether problem.
- `bluetoothd` logged the layer underneath:
  `record_cb() Unable to get Hands-Free unit SDP record: Connection refused`
  and `connect to <phone>: Connection refused (111)`. obexd reports a missing
  record whenever its SDP fetch is refused, so "Unable to find service record"
  can be a refusal with a misleading name.
- A full `Disconnect()` / `Connect()` cycle forced fresh SDP discovery. BlueZ
  still reported `0x1132`, `0x1133`, and `0x112f` afterwards, so those UUIDs were
  not stale, yet obexd still could not fetch the records. BlueZ's device
  discovery and obexd's record fetch can disagree even on fresh data.
- ANCS worked throughout. The LE/GATT half of the bond is healthy while the BR/EDR profile half is refused, independent.

Resolved on 2026-07-13. The cause was neither candidate listed at the time: 
the adapter's Class of Device had reverted to Computer/Laptop, so iOS
no longer treated the machine as an eligible accessory and refused the record
fetch.

Also worth knowing: a `Connect()` that times out leaves an attempt in flight, and
further attempts fail fast with `br-connection-busy`. Retrying through that state
prolongs it, which is why the bearer supervisor backs off exponentially
instead of retrying on a fixed interval.

### 2026-07-16 - Cause of the OBEX refusal

Same hardware as above. The phone was made to forget this computer and its Bluetooth
stack was reset, which cleared the condition and allowed a clean re-pair through
Tether's own `bt_pair` for the first time.

Class of Device is the cause of the refused record fetch. `btmgmt class 4 8` does
not persist across a `bluetoothd` restart! The adapter reverts to Computer/Laptop
(`0x...010c`). iOS declines to serve MAP and PBAP SDP records and the
refusal is a `Connection refused (111)` under obexd's misleading "Unable to find
service record". Restoring the class and re-pairing opened PBAP immediately. This makes
the CoD a runtime dependency, not a one-time setup step. Anything that restarts
`bluetoothd` silently breaks messages and contacts until it is set again.

The adapter must be `Pairable` for connect-first to work. Idles with `Pairable: no`. 
Because connect-first deliberately makes the iPhone the authentication initiator, BlueZ then refuses the inbound pairing request: the Linux side
displays its numeric comparison, iPhone never shows one at all, and fails with `br-connection-key-missing`. 
Tether now turns `Pairable` and `Discoverable` on for the duration of the transaction and restores both afterwards.

A stale record on the phone reproduces `br-connection-key-missing` indefinitely. A
failed authentication can leave iOS holding a record for the computer while Linux has
none. The bond must be removed on both sides (Forget This Device and `bluetoothctl remove`) before retrying. 
iOS shows no prompt in this state, which is what distinguishes it from a user-cancelled pairing.

Results of the successful transaction:

- Discovery -> `Device1.Connect()` on the unpaired device -> numeric check on both screens -> `Paired`, 
then the ANCS solicitation.
- `Device1` and `org.bluez.Bearer.LE1` both report `Paired` and `Bonded`, and
  `BREDR.Connected` and `LE.Connected` are both true. Connect-first produced the
  cross-transport bond on this controller, upgrading the previous line's inference to a capture.
- PBAP opened on the first attempt. MAP reported `Forbidden`, which is a permission
  rather than the transport refusal above, and exactly the distinction `classify_obex_error()` exists to make.
  Granting "Show Message Notifications" cleared it within one poll, with no re-pair and no session restart.
- Messages then listed from `telecom/msg/inbox` and threaded correctly.

Obexd API details, both on BlueZ 5.87 and neither matching what the D-Bus documentation says (shocker):

- `ListMessages` returns `(a{oa{sv}})`: dict keyed by message object path,
  not the `(a(oa{sv}))` array of structs that the API reference says. GDBus rejects the reply on type mismatch(!!?!)
- `MessageAccess1.SetFolder` walks from the session root. `"inbox"`
  alone fails with `Internal Server Error`. `"telecom/msg/inbox"` succeeds, as does
  `"/telecom/msg/inbox"` and a `"telecom"` step. `ListMessages` takes a folder arg relative to the current folder, so an empty string lists the folder
  `SetFolder` already selected.

Message listing fields as actually delivered: `Subject` (body),
`Timestamp` (ISO-8601) local time with no zone suffix (`20254816T509517`),
`Sender`, `SenderAddress`, `Recipient`, `RecipientAddress`, `Type` (`sms-gsm` for both SMS and iMessage), 
`Read`, `Sent`, `Folder`, `Size`, and `ConversationId`.
`ConversationId` present but zero on every message, confirming IT CANNOT BE a thread identifier(?!?).

`SenderAddress` is in whatever form the phone has (both bare national number (`5129328901`) and (`+15129328901`) in the same inbox (!?!).
Tether does not invent a country code, so the same person can appear as two threads if their number is in the phone both ways.

A message's D-Bus object path is not a stable identifier. obexd names messages
`/org/bluez/obex/client/session<N>/message<id>`, and `<N>` is the OBEX session number,
which increments on every reconnect. Same message observed as `session9/message…` and `session11/message…` across a restart.
Only the trailing `<id>` is stable, so it is what Tether dedupes and persists on. 
The full path is kept separately and refreshed on each re-listing, because it is what `Message1` calls must
address. Keying history on the path instead duplicates the entire mailbox on every reconnect.

### Sending

`MessageAccess1.PushMessage(sourcefile, folder, args)` takes a file, so the 
bMessage is staged in `$XDG_RUNTIME_DIR` mode 0600 and removed afterwards (`Charset` must be `utf8`, 
let obexd transcode to the native charset mangles anything outside the phone's default encoding.)

Success means the phone accepted the message, not that it delivered it. MAP reports
nothing back about delivery. Tether records its own sends locally, and that record is the
only evidence they happened. A push that times out is reported.

`TYPE` is always `SMS_GSM`, including for AppleID recipients: iOS decides between SMS
and iMessage on its own, and nothing on the Linux side can force or check that choice.

### ANCS

On the same hardware: after the LE bearer connects, BlueZ enumerates the ANCS
service and all three characteristics: Notification Source, Control Point and Data Source, under the device object, and `StartNotify` succeeds on both notifying
characteristics.

The permission is in Settings -> Bluetooth -> (i) -> Share System Notifications, and it is separate from the Message Notifications toggle that MAP needs.

Control Point responses have no request identifier, so only one request may be out at a time. And Data Source
responses carry no total length (?!?) and come back fragmented, so the only way to know
response has ended is to know exactly which attributes were requested. Those two facts
are why the sequencer exists.

`NotificationAttributeIDDate` (5) is requested for every notification, content
mirroring on or off -- a delivery time is metadata, not content. It is the phone's
local wall clock as `yyyyMMddTHHmmSS` with no zone, parsed by `parse_map_timestamp`
like a MAP timestamp. Some apps send none, and those fall back to the time the
attributes were fetched. Without it every notification carried the fetch time, so a
replayed backlog arrived stamped all alike -- see issue #48.

### Group messages

Off by default, `group_messages_enabled` in `$XDG_CONFIG_HOME/tether/bluetooth.json`. It also
needs `ancs_content_enabled` (on by default, `tether --bt-ancs-content off` to disable),
so group support cannot work without content mirroring.

MAP delivers a group message with one sender, no participant list and no conversation
identifier. The only other hint is to correlate Apple Messages ANCS notification:
its title is the sender, and its subtitle is either `To you & ...` for an unnamed group or
the group's name. Neither form contains a member list...

- Correlation is bounded to a 30-second window, and two notifications with the same
  text are refused instead of guessing, the wrong choice would put a message in the wrong conversation and send a reply at the wrong people.
- An unnamed group is repliable only when **every** participant name resolves to exactly one contact address. A name matching several contacts is refused.
- A named group stays read-only until we figure out the member list in `$XDG_CONFIG_HOME/tether/groups.json`.
    That list affects Tether's reply routing only and never modifies the group on the phone.
- iOS reports nothing when a member is added or removed, so an unknown sender is the only available signal that a member list has gone stale.
- Because iOS supplies a name but no conversation identifier, distinct named groups
  sharing a name collapse into one local thread. There is no way to tell them apart.

Repeated recipient vCards were observed to enter an existing iMessage group when the
recipient set matched, rather than fanning out into separate one-to-one threads. That is
an observation, not a guarantee.

### 2026-07-16: a reverted Class of Device does not revoke a granted session

Same hardware. `systemctl restart bluetooth` reverted the class from
`0x7c0408` to `0x7c010c` (Computer / Laptop), exactly as expected. MAP, PBAP, and ANCS
then reconnected and stayed up, with `tether --bt-status` reporting `class=wrong`
throughout.

So the class governs whether iOS offers and grants the Messages and Contacts permissions,
not whether it keeps honoring permissions it has already granted. The earlier refusal
recorded above happened while those grants were being established. Setting the class is
still required, just not continuously, which is why the breakage is delayed
and looks unrelated to the restart that caused it.

### 2026-07-16: LE bearer stuck on `InProgress`

Same hardware. After the daemon stopped and restarted twice within about twenty seconds while an LE bearer
connect was outstanding, every subsequent `Bearer.LE1.Connect` returned
`org.bluez.Error.InProgress` indefinitely. BR/EDR, MAP, and PBAP were unaffected and stayed up throughout, only ANCS was lost.

`bluetoothctl disconnect` on the device did not clear it, so the stuck state lives above
the ACL. `systemctl restart bluetooth` did clear it: LE connected and ANCS reported
`Notification mirroring is active` on the first attempt afterwards, with no other change.

The supervisor previously did its retry backoff on this error the same way it does on a
refusal. It now retries `InProgress` at the base interval and reserves the exponential growth for refusals,
which is what the phone actually sends when it declines. That does not fix the stuck state, but it stops the daemon from
sleeping through the recovery.

### 2026-08-19 - `Bearer.LE1.Connected` is false on a working ANCS link

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| BlueZ | 5.87, running with `--experimental` |
| Phone | iPhone 15 Pro |

Captured with notification mirroring demonstrably live -- `ancs: Notification
mirroring is active`, the full GATT tree enumerated under the device including
all three ANCS characteristics, notifications arriving:

- `org.bluez.Bearer.LE1.Connected` read **false** throughout, while
  `Paired` and `Bonded` on the same interface read true.
- The link was opened by the phone, inbound, in answer to the solicitation
  advert. BlueZ appears not to credit the LE bearer for a connection it did not
  initiate. An outbound `Bearer.LE1.Connect()` sets it true as expected.

So `Bearer.LE1.Connected` is evidence that LE is up, not evidence that it is
down. Trusting it alone reported "notifications are unavailable" over a session
that was delivering them, and kept the supervisor dialling a link that was
already open.

**Corrected later the same day.** The first version of this entry concluded that
the presence of the ANCS characteristics in the object tree was the better
signal, on the evidence that an LE-down device showed only `avrcp` and `sep1-6`
under its path. That evidence was from a boot in which no LE session had yet been
established. BlueZ caches a bonded device's attributes (`main.conf` `[GATT]
Cache`, default `always`), so once a session has existed the `service*` objects
persist across a disconnect, and reading their presence as a live link reported
LE up on a dead one -- which stopped the bearer supervisor dialling it at all.

Neither obvious property can be trusted alone:

| Signal | Fails when |
|---|---|
| `Bearer.LE1.Connected` | reads false on a live link the phone opened inbound |
| ANCS characteristics present | survive the disconnect, because BlueZ caches them |

`Notifying` on the Notification Source characteristic is the one that holds. A
notify session needs a live ATT link to exist, so it cannot outlive the link the
way a cached attribute does, and it was true throughout the working session
above. That is what `Device::le_link_up()` reads, and what anything tearing the
bearer down refuses to act against.

### 2026-08-19 - The solicitation advert was a one-shot

Same hardware. `LEAdvertisingManager1.ActiveInstances` sat at `0` on a machine
whose LE link had been down for hours, with BR/EDR, MAP and PBAP up throughout.

The advert carries `Timeout = 180`, BlueZ retires it and calls `Release`, and
nothing re-registered it: the only callers were `pair_device()` and
`--bt-solicit`. Since iOS reveals the notification permission, and dials the LE
link, only while it is broadcasting, a bond went permanently quiet three minutes
after pairing unless a human kept running `--bt-solicit`.

It is now driven from the connection loop: on air whenever BR/EDR is up, ANCS is
enabled and LE is down, re-armed each time BlueZ retires it, and unregistered as
soon as LE connects so the advertising instance is freed. A pairing transaction
still owns it outright while it runs.

Measured on the machine above, which had been in the broken state all day: the
daemon brought LE up 12 seconds after starting, with no manual step, and reported
`Notification mirroring is active`.

### 2026-08-19 - A subscription latch reported the phone's permission as missing

Same hardware, with all three iPhone toggles granted and verified.

`ancs_ready` stayed false with the reason "Waiting for notification access to be
allowed on the iPhone", which sends the user to settings that are already
correct. `Notifying` on both notify characteristics read false at the time.

`AncsClientState::subscribed` recorded that `StartNotify` had once succeeded, and
nothing re-read it. BlueZ clears `Notifying` when the LE link goes and restores
the characteristics without it, so the flag outlived what it described. The only
thing that used to clear it was `set_device()` seeing the device path change,
which the ANCS grace window and the cached GATT tree together prevented.

`tick()` now re-reads `Notifying` from BlueZ once per retry interval while
`ready` is false, and rebuilds the subscription -- or the whole discovery, when
the characteristics are gone -- rather than trusting the flag. The status text is
reached only with the subscription verified live, so it now names the toggle
(`Share System Notifications`) and means it.

### 2026-08-19 - A hung LE dial poisons the path until bluetoothd restarts

Same hardware, immediately after the above. Every `Bearer.LE1.Connect` was
*accepted* -- `ConnectResult::Requested`, no error -- and no link ever appeared,
while `StartNotify` on the same device answered `org.bluez.Error.InProgress`
indefinitely. `Bearer.LE1.Disconnect` succeeded and changed nothing.

This is worse than the `InProgress` case recorded on 2026-07-16. Three ways out
were tried against it directly, with the daemon stopped so nothing raced them:

| Attempt | Result |
|---|---|
| `Bearer.LE1.Disconnect` | `Not Connected`. Does not cancel a pending connect |
| `Device1.Disconnect` | Succeeds, BR/EDR returns within seconds, LE still `In Progress` |
| Waiting | `In Progress` indefinitely |

**`sudo systemctl restart bluetooth` is the only thing that clears it**, and a
bearer-level recovery built on `Bearer.LE1.Disconnect` was removed again after
this measurement rather than shipped as a remedy that does not remedy anything.

The consequence for the retry policy is the important part. A hung dial is not
free: it poisons `Bearer.LE1.Connect` for that device for the rest of
bluetoothd's life. Six dials after a clean restart were enough to do it. So the
cap on outbound LE attempts is deliberate and stays, and the link is expected to
come from the phone answering the solicitation advert instead -- which is why
that advert now stays on air the whole time LE is down. `reset()` re-arms the
attempts when the device or the Classic link returns, which includes every
bluetoothd restart, so a machine that recovers gets fresh dials without ever
accumulating them.

### 2026-08-19 - What an LE "connect" actually is, captured

`btmon` over a full session, including a clean re-pair. Two findings that change
how the LE half should be driven.

**`Bearer.LE1.Connect` does not dial.** Six Connect calls produced six
`Add Device` with `Action: Auto-connect remote device (0x02)`, one
`LE Add Device To Accept List`, one `LE Add Device To Resolving List`, eighteen
`LE Set Extended Scan Enable` -- and **zero** `LE Create Connection`. BlueZ
registers the phone for background auto-connect and waits for it to advertise.
An iPhone that is already BR/EDR-connected does not advertise, so the request
simply expires.

So the supervisor now holds exactly one registration open for as long as LE is
down (`BearerOps::le_connect_outstanding()`) instead of repeating a dial.
Repeating was what wedged BlueZ: the request runs for `LE_CONNECT_TIMEOUT_MS`
(45s) while the backoff re-fired after five, so registrations overlapped and
every later one answered `InProgress`. `Requested` and `Busy` are both the
healthy state now -- BlueZ is listening -- and only a refusal counts or backs off.

**The advertisement is correct**, which rules out a long-standing suspicion. It
goes out as `Use legacy advertising PDUs: ADV_IND`, connectable and scannable,
public address, 31 bytes exactly:

```
11 15 <ANCS UUID, little-endian>   solicitation, AD type 0x15
04 ff ff ff 00                     manufacturer data, company 0xffff
04 16 99 99 00                     service data, 16-bit UUID 0x9999
02 01 06                           flags: LE General Discoverable, BR/EDR Not Supported
```

with `Tether` in an 8-byte scan response. The 16-bit form of the service data is
what keeps it inside the legacy 31-byte limit. Nothing here needs trimming.

### 2026-08-19 - Cross-transport key derivation, captured

The re-pair produced a dual bond, and the capture shows exactly how. Immediately
after the BR/EDR link key lands, SMP runs **over the BR/EDR channel**:

```
HCI Event: Link Key Notification
BR/EDR SMP: Pairing Request    Authentication requirement: ... CT2 (0x20)
                               Responder key distribution: EncKey IdKey Sign
BR/EDR SMP: Pairing Response   Responder key distribution: EncKey IdKey
BR/EDR SMP: Identity Information          (the phone's IRK)
BR/EDR SMP: Identity Address Information
MGMT Event: New Identity Resolving Key
MGMT Event: New Long Term Key  Key type: Authenticated key from P-256 (0x03)
```

`Authenticated key from P-256` is the point: the LE LTK is derived from the
BR/EDR Secure Connections link key, which is why Secure Connections is a hard
precondition and why `--bt-status` reports it. The `CT2` bit in the
authentication requirements is what asks for the derivation.

**This is the check for a bond that has no LE half.** Capture a pairing with
`sudo btmon -w /tmp/pair.btsnoop` and look for `BR/EDR SMP: Pairing Request`
followed by `New Long Term Key`. If the SMP exchange never happens, or the key
type is not a P-256 one, the bond is BR/EDR-only and no amount of re-soliciting
or restarting will give it ANCS -- it has to be re-made, and Secure Connections
has to be on first.

The `Role Change ... Role: Peripheral` right before the exchange is also worth
noting: the iPhone takes the central role on the ACL, which is what connect-first
pairing exists to allow.

### 2026-08-19 - `le-connection-abort-by-local` was our own PreferredBearer write

After a reboot, LE would not come up at all: every attempt logged
`org.bluez.Error.Failed: le-connection-abort-by-local`, while the same
`Bearer.LE1.Connect` issued by hand succeeded immediately.

The difference was not the advertisement. The supervisor used to do this:

```
set PreferredBearer = "le"
Bearer.LE1.Connect()          <- asynchronous, returns at once
set PreferredBearer = "bredr" <- milliseconds later, connect still in flight
```

The second write lands while BlueZ is still setting the LE connection up, and
BlueZ abandons it -- which is exactly what `abort-by-local` says: the local host
cancelled it, not the phone.

`PreferredBearer` steers `Device1.Connect`. This path calls the per-bearer
`Bearer.LE1.Connect`, which already names the transport, so the property was
never needed here. (**Corrected 2026-08-25**: steering `Device1.Connect` is not
all it does. Left pinned to `"bredr"` it also keeps the inbound LE link from
forming -- see the entry at the end of this file.) It is now written only on the `Device1.Connect` fallback used
by BlueZ builds that publish `Bearer.LE1` without a `Connect` method. With the
writes removed, LE connected on the first attempt from a cold LE-down state and
held for a three-minute soak with zero aborts.

**An earlier reading of this was wrong and is corrected here.** The first
experiment compared "advert on air" against "advert off" and concluded that
advertising and initiating could not coexist without the kernel's Simultaneous
Central and Peripheral feature. That comparison was confounded: the advert was
only ever off while the *daemon was stopped*, so it was also the only condition
in which nothing was rewriting `PreferredBearer`. The advert was not shown to
interfere, and no evidence here says it does.

Tether still sequences the two -- the solicitation stays off air while outbound
attempts are being spent (`BearerStatus::le_dialling`) -- because doing one thing
at a time is cheap now that the connect succeeds on the first attempt. That is a
conservative choice, not a measured requirement.

### 2026-08-19 - The iPhone can go silent on LE with the permission still granted

After a long debugging session -- many connect attempts, several bluetoothd
restarts, a re-pair and a reboot -- LE stopped coming up at all, with everything
on the Linux side verifiable and correct:

- bond dual (`Bearer.LE1` Paired and Bonded), Secure Connections on, class ok
- `Share System Notifications` present **and on** in the iPhone's settings
- both routes provably running: the dial and solicitation phases alternating,
  the advertisement on air 180 seconds at a time
- phone unlocked, in range, BR/EDR + MAP + PBAP working throughout

The failure had changed shape, which is what identified it. A quiescent
`Bearer.LE1.Connect` -- daemon stopped, nothing advertising -- returned
`Connection timed out`, where the same call earlier in the day had returned
success. Not `abort-by-local` (this side cancelling), not `In Progress` (BlueZ
wedged): a properly issued request that nothing answered.

**Cycling Bluetooth off and on on the iPhone cleared it immediately.** The link
came up, ANCS subscribed, `Notifying` true on both characteristics, and a
three-minute soak held with no errors. Nothing on the Linux side changed.

So an iPhone will stop answering on LE while still showing the permission as
granted, and neither toggling that permission nor re-pairing is the remedy --
only cycling the phone's Bluetooth. This is the same shape as the BR/EDR refusal
recorded above, and likely provoked by the sheer volume of connect attempts the
session put at the phone.

Because the two causes need opposite remedies, the status text now separates
them: a transient failure reports that the iPhone is not answering and names the
phone-side Bluetooth cycle, while a refused registration still points at
`systemctl restart bluetooth`. Sharing one message for both is what sent this
session chasing the local stack for an hour.

### 2026-08-20 - Three ways the LE cycle sabotaged itself

A cold boot came up with BR/EDR, MAP and PBAP working and LE never forming, on a
machine that had soaked cleanly the evening before. The local stack measured
correct throughout: `bearer_api: confirmed`, `bond_has_le: true`,
`secure_connections: true`, CoD `0x7c0408`, and the solicitation advert on air
(`ActiveInstances` alternating 0 and 1 on the expected 45s/180s cycle). The
phone was the immediate cause -- a dial with nothing competing, against a freshly
restarted `bluetoothd`, returned `Connection timed out` in 25s, and 180s of
uninterrupted solicitation went unanswered. But reading the daemon log across a
full cycle turned up three faults of our own, each of which turns one bad minute
into a permanent outage.

**The solicitation aborted our own dial.** `connect_le()` is asynchronous, and
`le_dialling` was derived from the clock alone. When the 45s dial window closed
the advert went up immediately, while BlueZ was still connecting, and the connect
died with `le-connection-abort-by-local` -- observed once per cycle, forever. The
window now stays open while a dial is outstanding and closes on the reply, so the
two routes really do alternate instead of overlapping. `connect_le()` abandons
the call at its own timeout, which is what bounds the window.

**Dialling stopped for good after six failures.** `LE_ATTEMPTS_BEFORE_ADVICE`
gated the retry as well as the advice string it is named for, and `le_failures_`
is cleared only by `reset()` or by an observed LE link -- neither of which
happens while BR/EDR stays up. Six failures into a daemon's life, LE was never
dialled again. Combined with the abort above, that is roughly twenty minutes from
boot to a daemon that will not try. This is the "worked all morning, then stopped
until I restarted it" report. The cap now selects the message only; the retry
runs as long as LE is down, paced by the existing 300s backoff ceiling.

**PreferredBearer was written under a live LE link.** The Classic path called
`set_preferred_bearer("bredr")` on every reconnect attempt, including while LE
was up, which drops the LE link -- the mirror image of the race removed on
2026-08-19, where the LE path wrote `"le"` under an in-flight connect. A bug
reporter independently described the consequence exactly: the bond stays
`BR/EDR + LE` but only one bearer holds a live connection at a time, trading off
every few minutes. The property steers the untyped `Device1.Connect` and nothing
else, so it is now written only on that fallback, which is reached only with
nothing connected. It is gone from `BearerOps` entirely -- the supervisor cannot
touch it by construction, which is a stronger guarantee than the test it replaces.

None of the three explains a phone that will not answer. All three explain why it
never recovered once it stopped.

Cycling Bluetooth on the iPhone brought LE straight back -- first dial, no
aborts, both ANCS notify characteristics subscribed, clean over a soak. But the
daemon that had been failing all morning did not notice for minutes: LE shared
BR/EDR's 300s backoff ceiling, so a phone the user had just fixed still read as
broken. LE now backs off to its own 60s ceiling. A dial costs nothing since one
is never issued while another is outstanding, and the failure it recovers from
is the one a human has just cleared by hand.

### 2026-08-22 - The advert opens the link in 1.3s; the dial never opened one

Same hardware. Captured across a `bluetoothd` restart, a daemon start, and a
Bluetooth cycle on the iPhone, with the phone in the silent state described on
2026-08-19 and then brought back by hand.

The moment the link forms is unambiguous:

```
t=81.858   LE Set Extended Advertising Enable: Disabled          <- dial window opens
t=127.668  LE Set Extended Advertising Enable: Enabled, Handle 0x01, Success
t=128.962  LE Enhanced Connection Complete, Role: Peripheral, Success
```

**1.29 seconds.** `Role: Peripheral` means the iPhone was the central: it dialled
us, inbound, in answer to the solicitation. Peer address `59:71:0E:41:74:AA
(Resolvable)`, resolved to the identity address.

The dial had every chance and took none of it. Across three captures totalling
roughly 700 seconds, no `Bearer.LE1.Connect` produced an HCI connect attempt, let
alone a link -- BlueZ implements it as an accept-list-filtered passive scan, as
recorded on 2026-08-19. In this capture the phone was reported advertising at
t=75.0, 90.8, 91.1 and 92.6 while that scan had already been torn down by the
45s dial timeout at t=53.8, so nothing was armed when it was there.

So the two routes are not equals taking turns. One of them works in about a
second and the other has never worked here, while the dial window costs the
working route 45 seconds of airtime out of every 225 -- worse in practice,
because `reset()` on a Classic flap restarts the window. Measured over the first
127 seconds of this capture the advert was on air for 27.6 of them.

The supervisor now dials once per daemon and then leaves the radio to the
solicitation. `reset()` deliberately does not refund the dial: a Classic flap is
not a reason to take the advert down for another attempt. The dial is kept at all
only because a cold start is the one case with no evidence either way.

`BearerStatus::le_dialling` still gates the solicitation, so the advert cannot go
up while that single dial is in flight -- the abort recorded on 2026-08-20. It is
assigned *after* the attempt in the same tick, not before; reading it first let
the advert go up alongside the dial it was meant to protect.

Two leads closed. The advert's `Flags` byte reads `0x06` (`BR/EDR Not Supported`)
where the iPhone's own adverts read `0x1a`, on the same public address the phone
holds a BR/EDR link to. It is synthesised by the kernel, identical for every
BlueZ client, and `LEAdvertisement1` rejects a client-supplied AD type `0x01`
("Failed to parse advertisement"). It is not the differentiator. Separately, the
controller's resolving list is never programmed and the accept list gets a single
entry, yet the inbound RPA above still resolved -- not a fault either.

None of this explains a phone that will not answer. `[IdentityResolvingKey]` and
an LE-SC `[PeripheralLongTermKey]` with `Authenticated=3` were present throughout,
so re-pairing is not the remedy and was not needed; cycling Bluetooth on the
iPhone was, exactly as on 2026-08-19.

### PBAP

`Select("int", "pb")` followed by `PullAll` with `Format: vcard30` and `MaxCount` returned 456 contacts on the first attempt.

The transfer object disappears from D-Bus the moment it finishes, so a vanished object
is a normal terminal state rather than a failure (file may still take a bit to appear afterwards),
which is why the pull waits instead of giving up. Contacts are staged in `$XDG_RUNTIME_DIR` rather than `/tmp`, since a
phonebook is personal data.

### The store at rest

Mode 0600 was the whole protection until 0.2.24, and it is not enough: it does
nothing about `$HOME` backups, a synced home directory, or another process
running as the same user. Both stores are now sealed.

Each record is `base64(nonce[12] || ciphertext || tag[16])`, AES-256-GCM
through the OpenSSL that is already linked for TLS. The journal seals per line
so appending stays an append; the contact cache is rewritten whole after every
PBAP pull anyway, so it gets one envelope for the file.

The key is 32 bytes from `RAND_bytes`, kept in the desktop secret service
(schema `com.tether.Store`, attribute `application=tether`). Lookup uses
`secret_service_search_sync` **without** `SECRET_SEARCH_UNLOCK`:
`secret_password_lookup_sync` would raise an unlock prompt, which is wrong for a
user unit that can start before the graphical session. On a host with no secret
service on the bus at all the key falls back to `$XDG_CONFIG_HOME/tether/store.key`,
mode 0600 — that stops a backup or a synced home, not another process running as
you, and it is never written when a wallet is merely locked.

A key is generated and stored only when the search returns **zero** items and
the default collection is unlocked. `secret_password_store_sync` updates an
existing item whose attributes match rather than failing, so treating a locked
item as a first run would replace the key the sealed store was written with and
lose the history for good. The search therefore distinguishes four answers: a
readable secret, an item whose secret is null (locked — wait), no item at all
with the collection locked or missing (wait), and no item with the collection
unlocked (first run — generate). A failed search counts as locked. Waiting takes
the same one-a-minute retry as everything else here.

Three retention modes, `retention` in `bluetooth.json`, also `tether
--bt-retention`:

| Mode | Journal | Contacts | An older tetherd sees |
|---|---|---|---|
| `encrypted` (default) | `messages.ndjson.enc` | `contacts.json.enc` | nothing |
| `plaintext` | `messages.ndjson` | `contacts.json` | files it reads correctly |
| `none` | — | — | nothing |

The path follows the mode, and that is load-bearing rather than cosmetic. A
package upgrade leaves the running daemon on the old binary, and a downgrade can
put an old binary back permanently. An old `tetherd` cannot parse a sealed line,
so it would read an empty history and then compact that emptiness back over the
file. Because the only names it ever opens are `messages.ndjson` and
`contacts.json`, and those only ever hold plaintext, it finds nothing under
`encrypted` and writes to files nothing else reads. Rolling forward finds the
sealed store intact. `compact()` additionally refuses to replace a populated
journal with zero records, which closes the same hole for the next format change.

Changing modes migrates the data: read under the old mode, write under the new,
fsync, rename, then unlink the source — never the other way round, so a crash
mid-migration leaves both copies rather than neither. The next start finishes
the job: the journal folds any leftover source records into the destination and
unlinks it (duplicate handles collapse on replay, and `compact()` rewrites them
out), and the contact cache, being a whole-file snapshot, keeps the destination
and drops the source. Without that, a plaintext file stranded by a crash would
survive every later start. A store written by a pre-0.2.24 daemon migrates on
the first start after the upgrade.

A locked wallet keeps MAP and PBAP up and messages flowing to the UI, and only
pauses what is retained: the journal does not open, nothing is replayed, and
contact names go unresolved. The key is retried on each message poll, rate
limited to one wallet round trip a minute, so history starts persisting within a
minute of the user unlocking. A PBAP pull that happens while the wallet is
locked lives only in memory, since `save_contacts` will not write without a key.
Unlocking flushes that phonebook to disk instead of reading an empty cache over
it — the pull is once per PBAP session, so overwriting it would cost every
display name until the phone reconnects.

> **Losing the keyring entry loses the retained history.** There is no escrow and
> no plaintext copy left behind. The phone can refill part of it over MAP; sent
> messages, which the iPhone's MAP sent folder never returns, are gone. `tether
> --bt-retention plaintext` is the supported way to keep a readable copy.

## Credits

[BlueFerry](https://github.com/erikwb/blueferry) (Erik Bourget and contributors), `PROTOCOL.md` records the findings this implementation relies on.
Tether's Bluetooth support is an independent implementation written against those published
findings, Apple's ANCS specification, the Bluetooth SIG MAP and PBAP specifications, and
the BlueZ D-Bus API.

### 2026-08-23 - `Device1.Connect` cannot succeed without a locally connectable profile

iPhone 15 Pro, BlueZ 5.87 with `--experimental`, MediaTek MT7925, WirePlumber 0.5.

After a suspend/resume the daemon settled into a permanent retry loop, with messages,
contacts, and notifications all working the whole time:

```
[WARN] bluetooth: BR/EDR connect failed (GDBus.Error:org.bluez.Error.Failed: br-connection-unknown), retrying in 30s
[INFO] bluetooth: pulled 7 contacts
[INFO] ancs: Notification mirroring is active.
```

Captured, not inferred:

- `bluetoothd` logged exactly one profile attempt per retry, on the daemon's own
  5/10/20/30s cadence: `src/service.c:btd_service_connect() a2dp-source profile
  connect failed for <addr>: Protocol not available` -- `ENOPROTOOPT`.
- WirePlumber had registered only `/MediaEndpoint/A2DPSource/*`. Zero `A2DPSink`
  endpoints existed on that boot.
- `~/.config/wireplumber/wireplumber.conf.d/51-no-phone-audio.conf` held
  `bluez5.roles = [ a2dp_source hfp_ag bap_source ]` -- the file this document tells
  people to write, three sections up.
- `bluetoothctl info` read `Paired/Bonded/Trusted: yes`, `BREDR.Connected: no`.
- OBEX kept pulling contacts across the same window, which is proof the radio reached
  the phone while BlueZ called the device disconnected.

The mechanism: `a2dp-source` -- the iPhone as an audio source -- was the only
auto-connectable BR/EDR profile this machine had for the phone. With no local A2DP
sink endpoint BlueZ fails it, finds nothing else to connect, tears the ACL down, and
answers `Device1.Connect` with `br-connection-unknown`. BlueZ reports a device
Connected only while some local profile is connected, so on a host with none the flag
can never go true.

Tether read that flag as "the Classic link is up" and gated `ProfileSupervisor::tick()`
on it, so MAP and PBAP were never opened -- while `BearerSupervisor` retried a call
that could not succeed, forever. It had appeared to work before the resume only by
coincidence: obexd's own transient BR/EDR link made `Device.Connected` true for long
enough to latch (`bluetoothd: Device is already marked as connected`).

This is not one misconfigured desktop. Whether any local BR/EDR profile can connect to
a phone is a property of the host's audio and telephony stack, and the same dead loop
follows from a headless or container install with no PipeWire at all, a minimal
install without ofono so no HFP either, a distro shipping restricted `bluez5.roles`
defaults, or simply following the audio section above.

Fixed by treating a Classic link as an outcome the daemon reports, never a
precondition it waits on. `ProfileSupervisor` is now driven by `device_paired`:
`obexd` runs its own SDP query and transport connect, so a paired device is the only
precondition a session ever had, and an unreachable phone was already covered by the
`Unavailable` branch of the OBEX classifier. `BearerSupervisor::tick()` additionally
takes `obex_up`, an open MAP or PBAP session; after `CLASSIC_FAILURES_BEFORE_ADVICE`
consecutive refusals with the phone demonstrably serving OBEX, the status names this
computer's profile set instead of sending the user to cycle Bluetooth on a phone that
is answering.

What was deliberately **not** changed: the `LE_UP_CLASSIC_BACKOFF_MAX_SECONDS` ceiling
from 2026-08-22. An open OBEX session does not distinguish this case from the
transient iOS decline that entry describes -- OBEX is up in both -- so widening the
ceiling on `obex_up` would have restored the exact bug that ceiling was added to fix.
The 30s retry costs one D-Bus call; the retry rate was never the defect.

### 2026-08-23 - The GTK app hid the one line that said what to do

Same hardware, a boot right after the fix above. BR/EDR, MAP and PBAP came up in 1.3s
and LE never formed. The daemon behaved correctly throughout -- the advert was on air the
whole time, and at 181.2s the link reason escalated to exactly the right remedy:

```
t=    1.3s  Connected. Waiting for the iPhone to open the LE link...
t=  181.2s  The iPhone is not answering on LE. Its Bluetooth is wedged on its own
            side: turn Bluetooth off and back on on the iPhone...
t=  633.9s  classic=False            <- Bluetooth cycled on the phone
t=  644.2s  classic=True le=True     <- link forms on the way back
t=  651.1s  ancs=True                "Notification mirroring is active."
```

Ten and a half minutes of silence against a continuously broadcasting advert, cleared
instantly by the phone-side cycle. That is the 2026-08-19 failure exactly, and the
status text named it at the three-minute mark.

Nobody saw it. The GTK app picked `profile_reason` first and fell back to `link_reason`
only when it was empty -- and `profile_reason` was "Messages and contacts are connected."
So the app calmly reported the working half while discarding the only line that said what
to do about the broken one, for seven and a half minutes. The advice existed, was
correct, was on time, and was invisible where the user was actually looking.

A profile that is up is not news. The reason now follows the link: while BR/EDR or LE is
down the app shows `link_reason`, and `profile_reason` only once both are up.

Worth separating from the entry above: that one is an LE link that comes up carrying no
ANCS, this one is an LE link that never comes up at all. Same symptom in the app, and
they need opposite remedies -- which is the recurring lesson in this file.

### 2026-08-23 - A live LE link that carries no ANCS silences its own recovery

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| BlueZ | 5.87 |
| Phone | iPhone 15 Pro |

Reported as the CLI and the GTK app disagreeing about LE. They did not: both read the
same payload and both said LE was up. The ✗ was on the Notifications row, and
`--bt-connection` did not print that row at all, so the two could not be compared. That
is fixed here as well.

Captured from the daemon's own timeline, on a session that had been in the failed state
for over thirty minutes:

```
t=1.3s   BR/EDR up, MAP+PBAP up, LE down
t=29.7s  LE still down       ancs_reason "The iPhone is not connected over LE."
t=46.2s  BR/EDR dropped
t=54.5s  BR/EDR back
t=61.0s  le_connected -> TRUE   link_reason "Connected over BR/EDR and LE."
t=61.0s  ancs_reason -> "Waiting for the iPhone's notification service."   [stuck]
```

At t=61s the one-shot dial opened an LE link. Everything about it read healthy and it
carried nothing:

| Signal | Value |
|---|---|
| `Bearer.LE1` `Connected` / `Paired` / `Bonded` | all true |
| `Device1.ServicesResolved` | false |
| GATT objects under the device path | none |
| ANCS UUID `7905f431-...` in `Device1.UUIDs` | absent |
| `LEAdvertisingManager1.ActiveInstances` | 0 |

The solicitation was gated on `!le_connected`, so the dial's link took the advert off
air permanently -- and the advert is the only thing that asks the phone for ANCS.
`AncsClient::discover()` scanned for characteristics that were never going to appear and
retried behind "Waiting for the iPhone's notification service." forever. This is the
daily failure that had been cleared by hand, and the fiddling that cleared it was
breaking the dead link so the advert could go back up.

**`tether --bt-solicit` alone fixed it, with nothing touched on the phone.** Under a minute:

| | Before | After |
|---|---|---|
| `ancs_ready` | false | true |
| `ancs_reason` | "Waiting for the iPhone's notification service." | "Notification mirroring is active." |
| ANCS UUID in `Device1.UUIDs` | absent | present |
| `ServicesResolved` | false | true |

So the iPhone was not withholding ANCS, and no permission was wrong. It was never asked.
The advert now goes back on air over an LE link that has stayed up for
`ANCS_ABSENT_GRACE_SECONDS` without the phone offering the service, which is
`should_solicit_ancs()`. The window is 75s -- longer than the GATT discovery measured on
2026-08-22, so a normally forming link is never solicited over. A dial in flight still
holds the advert off (2026-08-20), and a live subscription still outranks every property
(2026-08-19), so neither recorded failure is reintroduced.

This is the 2026-08-19 lesson in the other direction. `Bearer.LE1.Connected` reads false
on a live ANCS link and true on a link that carries none: it is not evidence either way,
and gating on it in either polarity is what breaks.

Inferred, not captured: that the dial rather than an unanswered advert produced the dead
link. The t=61s transition and the measurements on 2026-08-22 -- where the advert opened
a link in 1.3s and the dial never opened one -- are what point at it. The fix does not
depend on which opened it.

### 2026-08-22 - Coming back in range waited on a backoff nothing could clear

Left the laptop, came back, and the log had been idle for minutes:

```
[WARN] bluetooth: BR/EDR connect failed (br-connection-unknown), retrying in 5s
[WARN] bluetooth: BR/EDR connect failed (br-connection-unknown), retrying in 10s
... 20s, 40s, 80s, 160s ...
[WARN] bluetooth: BR/EDR connect failed (br-connection-unknown), retrying in 300s
```

Two halves each waiting on the other:

**The advert was gated on the Classic link.** `supervise_ancs_solicitation` required
`bearer.classic_connected`, so with BR/EDR down nothing was on air. The advert is the
only part of this daemon that reacts to the phone returning -- the iPhone opens the LE
link 1.3s after seeing it (2026-08-22 above) -- and it was switched off in exactly the
state where it is the sole means of recovery. The gate no longer mentions BR/EDR.

**The Classic backoff had no way to learn the phone was back.** `br-connection-unknown`
is BlueZ's catch-all for a failed Classic connect: it covers a phone that walked out of
range and a phone that refused, so it cannot be added to `is_transient` -- doing that
would page a wedged phone every 5s forever. The backoff to 300s is right while the phone
is genuinely gone. What was missing was the event that ends the absence. An LE link
opening is that event: it is proof the phone is in range and answering. `tick()` now
clears `classic_backoff`, `next_classic_attempt_` and `classic_failures_` on the
false-to-true edge of `le_connected`, so BR/EDR is dialled on the same tick.

Clearing `classic_failures_` matters on its own: six failures out of range had the status
telling the user the iPhone "keeps refusing the Bluetooth connection", advice for a wedged
phone, about a phone that had only been in another room.

Together: return to range, LE comes up within seconds off the advert, Classic follows
immediately. Neither half waits on the other any more.

### 2026-08-22 - A live LE link is proof, not an event

The first cut of the fix above cleared the BR/EDR backoff on the false-to-true edge of
`le_connected`. Walking back in showed why that is not enough:

```
BR/EDR connect failed (br-connection-unknown), retrying in 5s
... 10s, 20s, 40s, 80s ...
```

all of it with LE connected and notifications mirroring. The edge fired once, and the
backoff then climbed unopposed against a phone that was demonstrably in range and
answering. iOS declines the BR/EDR page for its own reasons while holding the LE link up;
that is a "not now", not an absence, and it clears on its own within seconds.

The backoff ceiling is now conditional: `LE_UP_CLASSIC_BACKOFF_MAX_SECONDS` (30s) while LE
is connected, the full 300s only when it is not. The edge clear stays -- it still collapses
a ceiling grown during a real absence the moment the phone answers.

The same state produced worse advice. With six failures logged, `--bt-status` said the
iPhone "keeps refusing the Bluetooth connection. Turn Bluetooth off and back on" -- about a
phone that was delivering notifications over LE at that moment, and where following the
advice would have broken the one bearer that was working. That string is now suppressed
while `le_connected`, replaced with one that says what is actually true: notifications are
connected, messages and contacts are still being reached for.

**A dead OBEX session was read as a phone capability.** The same outage produced:

```
the sent folder is unavailable (Transport got disconnected); showing received messages only
listing 'inbox' failed (UnknownObject: Method "ListMessages" ... doesn't exist); falling back to the inbox alone
```

Both are capability fallbacks -- `map_lists_sent` and `map_lists_subfolders` are latched
off for the life of the session -- and both fired on an OBEX session that had simply gone
away with the link. A dropped link left the daemon convinced the iPhone serves no sent
folder and no subfolders. `map_session_dead()` now separates the two: `UnknownObject` and a
disconnected transport skip the fallbacks entirely and reopen the session on the spot,
rather than counting to `MAP_FAILURES_BEFORE_REOPEN` first.

### 2026-08-22 - The advert was not the problem; the per-bearer Connect was

A daemon start with the phone on the desk logged `br-connection-unknown` on repeat while
ANCS was already talking GATT over LE. The obvious suspect was the advert change above --
connectable LE advertising now runs during the window BR/EDR is being paged, which it never
did before. Measured instead of assumed, with `tetherd` stopped so nothing else drove the
radio:

| Condition | Cold `Device1.Connect` |
|---|---|
| No advertising | 1s, 2s, 1s -- 3/3 |
| ANCS solicitation on air | 1s, 1s, 1s -- 3/3 |

The advert costs nothing. Paging works fine underneath it, and the gate change stands.

What differs is which method is called. `connect_classic()` picks the per-bearer
`Bearer.BREDR1.Connect` whenever `Device1.Connected` is already true, and treats a refusal
from it as final. Before the advert change that branch was nearly unreachable at startup:
with no advert, LE never came up first, so `Device1.Connected` stayed false and the untyped
`Device1.Connect` -- the one that connects in a second -- did the work. Now LE comes up
within seconds of the advert going on air, `Device1.Connected` flips to true, and every
subsequent Classic attempt takes the per-bearer path, which answers `br-connection-unknown`
under a live LE link.

So the advert did not break paging; it changed which door the daemon knocks on. A refusal
from the per-bearer Connect now falls through to `Device1.Connect` instead of ending the
attempt. Only `InProgress` and `AlreadyConnected` still return early -- both mean a connect
is already running, and racing a second one against it is the failure mode that produced
the `InProgress` storms.

`set_preferred_bearer("bredr")` sits on that fallback path, and reaching it under a live LE
link would reintroduce the 2026-08-20 bug exactly. It is now skipped whenever LE is up. The
property only steers the untyped Connect, and with LE up the phone is reachable without it.

### 2026-08-22 - Confirmed: a clean start, and where the remaining minute goes

First daemon start after the fallback above, phone on the desk:

```
[INFO] bluetooth: soliciting ANCS for 180s
[INFO] bluetooth: pulled 7 contacts
[INFO] ancs: the notification subscription is no longer live (BlueZ cleared Notifying), rebuilding it
[INFO] ancs: StartNotify not ready yet (org.bluez.Error.InProgress: In Progress)
[INFO] ancs: Subscribing to the iPhone's notifications...
```

Not one `br-connection-unknown` in the whole run, and PBAP pulled before any warning was
logged at all -- BR/EDR was up almost immediately. The backoff climb that opened this
investigation is gone.

What is left takes about a minute, and it is entirely the LE half: `StartNotify` answering
`InProgress` while BlueZ finishes GATT discovery on the freshly opened link. The daemon
retries and it clears on its own.

**This is not the hang recorded on 2026-08-19.** That one also answers `InProgress` on
`StartNotify`, and the difference matters because the remedy there is a `bluetoothd`
restart, which is worth nothing here:

| | Transient (normal startup) | Hung (2026-08-19) |
|---|---|---|
| Duration | Seconds to about a minute, then subscribes | Indefinite |
| LE link | Up -- the characteristics are there to discover | Never appears; every `Bearer.LE1.Connect` is accepted and nothing connects |
| Remedy | None. Wait | `sudo systemctl restart bluetooth` |

The link being up is the discriminator: `InProgress` under a live LE link is discovery
still running, not a poisoned path.

### 2026-08-25 - Connect-first is refused transiently, and the fallback costs the LE half

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone 15 Pro, iOS 26 |
| Adapter class | `0x00580408` -- A/V Hands-Free |

Prompted by [#49](https://github.com/zackb/tether/issues/49), where a Realtek `0bda:a728`
never gets past the connect step: the only line the phone's refusal produces is a profile
connect failing, and no passkey, agent, or link-key line ever follows.

```
src/profile.c:ext_connect() Hands-Free unit failed connect to <phone>: Connection refused (111)
```

That reproduced here, on the hardware every earlier entry in this file was recorded on, with a
different errno and the same meaning. Two pairings, both from a bond deleted on Linux and
forgotten on the iPhone, six minutes apart:

**Run 1 -- connect-first refused, fallback bonded.**

```
bluetoothd: profiles/audio/hfp-hf.c:connect_cb() connect to <phone>: Connection reset by peer (104)
  connecting   -> confirm 295008   -> no bond           (iPhone: "Pairing Unsuccessful")
  retrying     -> Device1.Pair()
  pairing      -> confirm 329348   -> Paired
```

Result: `Bond: BR/EDR only`. `bluetoothctl info` reported `BREDR.Paired`, `BREDR.Bonded` and
`BREDR.Connected` with no `LE.` counterparts at all. MAP and PBAP both opened; ANCS could not
exist on that bond.

**Run 2 -- same machine, same phone, connect-first succeeded on the first attempt.**

```
  connecting   -> confirm 491968   -> Paired
```

Result: `Bond: BR/EDR + LE`, `Bearer API: confirmed`.

Three things this settles:

- **An explicit `Device1.Pair()` yields a BR/EDR-only bond.** This was an inference carried in
  a comment on `AuthStrategy` since the strategy was written; it is now captured. The
  cross-transport derivation needs the iPhone to be the authentication initiator, which is the
  whole reason connect-first exists. So the fallback buys messages and contacts at the price of
  notifications, and is a last resort rather than an equal alternative.
- **The iPhone's refusal of connect-first is transient.** Same controller, same phone, same
  clean starting state, opposite outcomes minutes apart. Falling back on the first refusal
  therefore trades ANCS away for a failure that would have cleared on its own. Connect-first is
  now attempted twice before the fallback is spent.
- **Remembering the fallback is a trap.** The first build persisted `auth_strategy` as whatever
  bonded, so a single refused attempt latched `explicit-pair` into the config and every later
  re-pair skipped the only path to the LE keys -- with nothing in the UI to undo it. The winning
  strategy is now persisted only when the bond it produced was dual.

Not captured: why the phone refuses. Both refusals landed on a profile connect (`hfp-hf` here,
`Hands-Free unit` in #49) and both left the phone showing "Pairing Unsuccessful", which is
consistent with iOS declining an unbonded peer's profile connect and tearing the ACL down before
any security procedure runs -- but nothing here rules out a controller or firmware cause, and no
`btmon` capture was taken of the refusal itself.

### 2026-08-25 - The bond was pinned to BR/EDR, and the LE half never formed

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone 15 Pro, iOS 26 |

A fresh dual bond sat with `LE: no` for over twenty minutes. Everything the earlier entries
tell you to check was already right: `class=ok`, `secure-connections=on`, `Bond: BR/EDR + LE`,
`Bearer.LE1` reporting `Paired` and `Bonded`, the solicitation confirmed on air at the
controller (`LEAdvertisingManager1.ActiveInstances: 1`), and `bluetoothd` logging no dial error
of any kind -- no `InProgress`, no `abort-by-local`. Cycling Bluetooth on the iPhone, the remedy
the 2026-08-19 and 2026-08-23 entries prescribe, did nothing: Classic dropped and came back, LE
stayed down.

The cause was this computer's own bond state. `pair_device()` wrote
`PreferredBearer = "bredr"` after pairing -- to bring Classic up first and let the ACL settle --
and never cleared it. The pin stands for the life of the bond.

Captured as an A/B on the live bond, with the phone untouched throughout:

| `PreferredBearer` | LE after `Bearer.LE1.Disconnect()` |
|---|---|
| `bredr` | down for 180s, twelve consecutive polls |
| `le` | up within 12s, `ServicesResolved` true, held 120s |

**This corrects the 2026-08-19 reading that the property "steers the untyped `Device1.Connect`
and nothing else."** The link the iPhone opens is inbound -- 2026-08-22 captured it as
`Role: Peripheral`, the phone dialling us in answer to the solicitation -- and no outbound
`Device1.Connect` is involved at all. A bond pinned to `bredr` does not accept that inbound
dial. The exact mechanism inside BlueZ was not captured; the behaviour was, twice.

Fixed by handing the preference back after the Classic settle: `pair_device()` writes `"bredr"`,
waits out `CLASSIC_SETTLE_SECONDS`, then writes `"le"` before soliciting. Verified on a fresh
pair with no manual step -- `PreferredBearer` read `"le"` straight out of the transaction, LE
came up at t=12s, ANCS at t=24s, and all six rows of `--bt-connection` read yes. The same code
path before the fix had left LE down for twenty minutes on the same hardware and phone.

Two things this does **not** cover, both untested rather than ruled out:

- **Bonds made by older builds stay pinned.** Nothing clears `"bredr"` on an existing bond, so
  they need a re-pair. Correcting it from the supervisor instead is the obvious fix and is
  deliberately not written: writing this property from the supervisor is what caused
  `le-connection-abort-by-local` on 2026-08-19 and the LE drop on 2026-08-22, so it wants a
  measurement, not an assumption.
- **`connection.cpp` can re-pin `"bredr"`** on the Classic `Device1.Connect` fallback. That path
  is reached only with nothing connected, but it can undo the fix later in a session.

Also captured, and worth separating from all of the above: **whether connect-first derives the
LE keys at all is itself flaky.** Same machine, same phone, same code path, two fresh pairs
forty minutes apart -- 19:38 gave `BR/EDR + LE`, 20:25 gave `BR/EDR only`, with
`secure-connections=on` and `--experimental` active for both. Until that is understood, the real
procedure after pairing is to read `Bond:` and re-pair until it says `BR/EDR + LE`.

One incidental: `StartDiscovery` began failing with `org.bluez.Error.InProgress` while
`Adapter1.Discovering` read false and `StopDiscovery` answered `No discovery started`. Only a
`bluetoothd` restart cleared it, matching the auto-connect wedge already in the troubleshooting
table. `tether-btclass@hci0` restored the class across that restart without intervention, after
the adapter briefly came back `Powered: no` with `Class: 0x00000000`.

### 2026-08-26 - A confirmation nobody could see counted as a refusal

| | |
|---|---|
| Controller | Reported on Realtek `0bda:a728`; mechanism captured on MediaTek MT7925 (RZ717) |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone SE, iOS 27.0 beta (reporter); mechanism is phone-independent |

Issue #49, after the explicit-pair fallback shipped. Pairing now reached the numeric comparison
and the iPhone displayed a code, but the bond never completed. The reporter's timeline:

```
    519 ms  pairing      <phone>
   1258 ms  confirm      085363
  91715 ms  bt_pair_result   (fail)
```

91715 - 90000 (`PAIR_TIMEOUT_SECONDS`) = 1715. `Device1.Pair()` returned an error roughly 450 ms
after the agent asked for confirmation. That is a dialog dying, not a person deciding. Two
consecutive `confirm` steps 468 ms apart with different passkeys are the phone restarting SSP
after the rejection, not the passkey "regenerating".

Captured locally, no phone involved:

```
$ env -i HOME=$HOME tether-dialog --title t --body b --accept ok --reject no --timeout 3
Gtk-WARNING **: cannot open display:
exit=1
```

`gtk_init` failed and GTK exited 1. `confirm_with_dialog()` treated every non-zero exit as a
refusal, so the agent answered BlueZ `RequestConfirmation` with "Rejected by the user" within
milliseconds. **A `tetherd` with no display auto-declined every pairing.** It also explains the
attempts that went to "Pairing Unsuccessful" without ever offering Pair: the rejection landed
before iOS finished drawing the prompt.

The correlation to #49 is inferred from that arithmetic; the mechanism is captured.

Three things were wrong, and all three are fixed:

- `tether-dialog` now uses `gtk_init_check()` and exits 3 -- the code already reserved for a
  display failure -- instead of exiting 1, which is indistinguishable from the reject button.
- "Could not ask" is no longer "the user said no". It does not set `user_rejected`, it does not
  suppress the connect-first retry by pretending to be a refusal, and it does not silently
  accept either: bonding without the comparison is exactly what the comparison exists to prevent.
- The question is routed to the client that started the transaction. The daemon broadcasts
  `bt_pair_confirm_request` with the code and blocks up to 60s for a `bt_pair_confirm` answer.
  `tether --bt-pair` prompts on the terminal; the GTK app opens a dialog. A daemon that can show
  its own dialog still does, so nothing changes for a `tetherd` started from a desktop session.

If neither a dialog nor a client can be reached, the result now says so and names the fix,
rather than reporting a rejection that never happened.


### 2026-08-29 - A missing library declined every pairing

| | |
|---|---|
| Controller | Realtek `0bda:a728` (reporter) |
| BlueZ | 5.87, running with `--experimental` |
| Compositor | niri (Wayland), Arch Linux |
| Package | `tether-bin` 0.2.17 |
| Phone | iPhone SE, iOS 27.0 beta |

Issue #49, resolved. The reporter's `tetherd.log` showed, on every attempt, immediately after
`confirm <code>`:

```
tether-dialog: error while loading shared libraries: libgtk-layer-shell.so.0: cannot open shared
object file: No such file or directory
```

`tether-bin` did not list `gtk-layer-shell` in `depends`, so `/usr/bin/tether-dialog` never started:
the dynamic loader killed it with exit 127 in milliseconds. `confirm_with_dialog()` treated only
exit 3 and a signal as "could not ask", so 127 counted as a refusal and the agent answered BlueZ
"Rejected by the user" ~400 ms after the phone showed its code. The 90s the reporter saw was
Tether's own `PAIR_TIMEOUT_SECONDS` poll running out after the failure, not a stall.

`bluetoothctl` bonded on the same hardware throughout the thread because it uses its own agent and
never launches `tether-dialog`. The controller was never the problem.

After `pacman -S gtk-layer-shell`, `tether --bt-pair <addr> --explicit-pair` bonded on the first
attempt, with MAP and PBAP both working and contact names resolved.

Fixed:

- `gtk-layer-shell` is declared in both PKGBUILDs. The DEB and RPM dependency lists already had it.
- Only exit 0, 1 and 2 -- accept, reject, timeout -- now count as an answer. Every other exit, and
  any signal, means the dialog answered nothing, which routes the comparison to the CLI or GTK
  client. The previous fix only covered the one exit code it knew about.
- `tether-dialog` checks `gtk_layer_is_supported()` before `gtk_layer_init_for_window()`, which
  otherwise aborts where the compositor has no `wlr-layer-shell`, and falls back to an ordinary
  centered window.

**Explicit-pair bond, captured:** the bond came up `BR/EDR only` -- MAP and PBAP work, the LE half
did not derive. This is the first capture of what a `Device1.Pair()` bond yields, and it matches
what the `ConnectFirst` comment in `config.hpp` claimed without evidence. One sample, on a
controller whose cross-transport derivation is already known to be flaky, so it is not yet proof
that explicit-pair cannot produce a dual bond.

### 2026-08-29 - Turning mirroring off took messages down with it

Reported as #64: unchecking "Mirror iPhone notifications" stopped message send and receive, which
read as notifications being a dependency of messages. They are not -- ANCS rides LE, MAP and PBAP
ride BR/EDR OBEX -- and two separate things made it look like one.

`bt_set_ancs` applied the preference by restarting the whole supervision stack
(`set_device()` -> `stop()` + `start()`), so a setting about LE tore down the OBEX sessions
messages ride on. `stop()` never called `profiles->reset()` and `ProfileSupervisor` had no
destructor, so those sessions were abandoned at obexd rather than removed. The iPhone serves one
MAP session at a time, so the reopen could come back `Connection refused (111)` or `forbidden`,
and `send_message()` then reported "Messages are not connected" until `tetherd` restarted. Every
`set_device()` caller leaked the same way: pairing, the Bluetooth on/off checkbox, and the
capability re-read that fires on BlueZ events.

Fixed:

- `ProfileSupervisor` releases both sessions in its destructor. That covers every path that drops
  one, not just this toggle.
- `ConnectionManager::set_ancs_enabled()` records the preference; the supervisor thread applies
  it on its next tick -- the bearer supervisor's flag, the ANCS client, and the solicitation. The
  profile supervisor is not touched. The preference and the controller's capability are re-read
  every tick, so `refresh_capability()` had nothing left to do and is gone.
- Group replies follow `ancs_enabled`, not only `ancs_content_enabled`. With mirroring off the
  correlator is never fed, so a group thread that still advertised a reply route could only fail.

### 2026-09-01 - Mirroring latched itself off, and pairing ran on the wrong controller

Reported as #69 by two people, two unrelated causes, one symptom each.

**A BR/EDR-only bond turned notification mirroring off, permanently and invisibly.**
`run_bt_pair()` persisted `ancs_enabled = false` whenever a transaction it ran produced a bond
without an LE half. `should_solicit_ancs()` is gated on that flag, so the solicitation advert
never went on air again -- and iOS reveals the "Show Notifications" toggle only while a bonded
peer is soliciting ANCS. The reporter's words were that the toggle never appeared. It could not:
the phone was never asked.

Nothing surfaced the state either. `--bt-status` had no row for it, `--bt-devices` no flag; only
the GTK checkbox read it. The recovery a second reporter found by hand -- `tether --bt-ancs on`
then `tether --bt-solicit` -- is the only one that existed.

This is the same trap the 2026-08-25 entry records for `auth_strategy`: a failed attempt writing
its own failure into the config, with nothing in the CLI to undo it. The `ancs_enabled` copy of
it was missed at the time.

Fixed by deleting the latch. A dual bond still turns the preference on; nothing turns it off but
the user. The runtime already gates ANCS on `ancs_available()` and on `bearer.le_available`, so a
BR/EDR-only bond costs one advert that the iPhone ignores, not a permanent silence. `--bt-status`
grew a `Notifications:` row that names `tether --bt-ancs on` when it is off.

**Everything ran on `hci0`, which was a Cambridge Silicon Radio clone dongle.** The reporter had
that dongle and an Intel 9460/9560; `resolve_capability()` took the first *powered* adapter from a
path-sorted list, and `pairing.cpp` took `adapters.front()` at six sites with no powered check at
all -- so the two could also disagree about which controller they were describing. There was no
setting, no flag, and `--bt-status` printed adapter addresses without saying which one was in use.
He found it by accident, unplugged the dongle, and pairing worked on the first attempt.

Fixed with `preferred_adapter(objects, id)`, one picker for both: the configured controller when
present, else the first powered, else the first. `Config::adapter` holds an `hciN` or an address,
`tether --bt-adapter <hciN|auto>` sets it, and `--bt-status` marks the controller in use and says
when a pinned one is absent. `pairing.cpp` routes all six sites through it.

Not captured: whether the CSR dongle can derive the LE keys at all. It reported `class=ok`,
`secure-connections=on` and both LE roles, and still produced only BR/EDR bonds across several
attempts -- consistent with the clone firmware these dongles are known for, but no `btmon` capture
was taken.

### 2026-09-01 - `btmgmt` never runs its command when stdin is `/dev/null`

Reported as #93 by several people: `sudo systemctl enable --now tether-btclass@hci0` hangs. The
unit sits in `activating (start)` indefinitely with `btmgmt --index hci0 class 4 8` alive on 4ms of
CPU, while `sudo btmgmt class 4 8` typed by hand succeeds instantly.

`bt_shell_attach()` in BlueZ's `src/shared/shell.c` registers stdin with the mainloop *before* it
looks at whether the shell is interactive:

```c
input = input_new(fd);        /* -> io_new(fd) -> mainloop_add_fd(fd, 0, ...) */
if (!input)
        return false;         /* returns here; shell_exec() never runs */

if (data.mode == MODE_INTERACTIVE) {
        ...
} else {
        if (shell_exec(data.argc, data.argv) < 0)
```

`mainloop_add_fd()` ends in `epoll_ctl(EPOLL_CTL_ADD, fd)`. `/dev/null` has no `.poll` in its file
operations, so the kernel answers `EPERM` -- an event mask of `0` does not help, the rejection is
about the file, not the events. `io_new()` returns NULL, the attach bails out one line before the
branch that would have run the command, and the mainloop then runs forever with nothing registered
to wake it. The process is not slow or blocked on the controller; it never issued the command.

systemd gives every service `StandardInput=null`, so both `btmgmt` calls in the unit inherited
`/dev/null` on fd 0. Terminals, pipes and sockets are all pollable, which is why running it by hand
works, and why `RLovelett` found that `printf "\n" | btmgmt ...` works around it.

It only bites where BlueZ is built against `src/shared/mainloop.c`. The `mainloop-glib.c` build goes
through `g_io_add_watch`, which accepts `/dev/null` -- so it reproduces on the reporters' Ubuntu
26.04 and not on Arch's bluez 5.87.

`probe_secure_connections()` had the quiet version of the same bug: it passed `nullptr` for
`standard_input` to `g_spawn_async_with_pipes`, so `btmgmt info` inherited whatever fd 0 `tetherd`
had. Launched from a `.desktop` entry that is `/dev/null`, the child hung, the 1s deadline killed
it, and `--bt-status` printed `secure-connections=unknown`. The reporter's paste shows exactly that
line.

Fixed by giving `btmgmt` a pipe everywhere it is spawned: `echo |` in the unit, in the NixOS module
and in `bt-probe.sh`, and a real stdin pipe closed immediately for EOF in `probe_secure_connections()`.
The unit also grew `TimeoutStartSec=60`, because `Type=oneshot` defaults to no start timeout -- that
is why a stall wedged `bluetooth.service` and everything ordered after it instead of failing.

### 2026-09-02 - `tetherd` segfaulted every time supervision was restarted

Reported as #110: three SIGSEGVs in ~20 hours, always with the iPhone (re)connecting while
supervision was restarting. The core showed `~ProfileSupervisor` running inside
`ConnectionManager::start()` on one thread and `stop()` joining the worker on another, with the
destructor reading unmapped memory.

The concurrency in the core was real but not the fault. `start()` replaced the per-device objects
in the wrong order:

```
state_->profile_ops = std::make_unique<ObexProfileOps>(address);          // frees the old ops
state_->profiles    = std::make_unique<ProfileSupervisor>(*profile_ops);  // destroys the old supervisor
```

`ProfileSupervisor` holds `ProfileOps&` and its destructor calls `reset()`, which removes both
obexd sessions through that reference -- the destructor added on 2026-08-29 so sessions are not
abandoned. `stop()` cleared every other piece of per-device state but left these four
`unique_ptr`s alone, so on every `set_device()` after the first the old supervisor was still
alive when `start()` freed the ops underneath it. The destructor then read `conn_` out of freed
memory and made a D-Bus call through it. That is a deterministic use-after-free on a restart, not
a rare race, and the two earlier `error 15` crashes are the same object reached through a
dangling pointer.

Fixed:

- `stop()` releases the per-device objects in dependency order -- supervisors, then the ops they
  borrow -- so `start()` only ever builds into empty pointers.
- `start()`, `stop()` and `set_device()` are serialized on a lifecycle mutex, held across both
  halves of a restart. Every caller reaches them from a detached thread (`run_bt_pair()`,
  `restart_supervision()`), and nothing had ordered them. The supervisor thread never takes it,
  so `stop()` can join under it.
- The ANCS client is a `shared_ptr` behind its own mutex. `notifications()` and
  `perform_notification_action()` are served on a command thread while the supervisor thread
  rebuilds the client in `apply_ancs_preference()` and `stop()` releases it; a caller now holds
  the client alive for as long as it is inside it. `AncsClientState::ready` is atomic for the
  same reason.

Still open: `AncsClientState`'s GATT signal callbacks run on the GLib thread and are unsubscribed
from whichever thread destroys the client, so a callback in flight during teardown remains a
narrow window. It needs the unsubscribe routed through `BluezMonitor::invoke_sync()`.

### 2026-09-03 - An adapter with no `LEAdvertisingManager1`, and three futile re-pairs

Reported as #118, a redacted `bt_diagnostics` dump with no prose. Every field below
is from that dump; the controller, kernel, BlueZ version, phone model and iOS version
were **not captured** -- the report carried none of them, which is half the finding.

| | |
|---|---|
| Controller | not captured |
| Kernel | not captured |
| BlueZ | not captured; running with `-E` (`bearer_api: unknown` rather than `absent`) |
| Adapter roles | central + peripheral; **0** advertising instances |
| Adapter class | `0x7c0908`, reported `class_ok` |
| Phone | not captured; MAP and PBAP both connected, so an iPhone |
| Tether | 0.2.23 |

Every pairing attempt logged the same warning:

```
Could not advertise for ANCS: GDBus.Error:org.freedesktop.DBus.Error.UnknownMethod:
Method "RegisterAdvertisement" with signature "oa{sv}" on interface
"org.bluez.LEAdvertisingManager1" doesn't exist
```

`UnknownMethod` means the object exists and the interface on it does not. BlueZ
registers `LEAdvertisingManager1` only from `read_adv_features_callback()`, after
`MGMT_OP_READ_ADV_FEATURES` returns a non-zero max advertising length, so a
controller that reports no LE advertising support gets no interface -- not an
interface whose instances are busy. `advertising_instances: 0` in the same dump is
consistent with both readings, which is why the two were not separable from the
report alone.

The consequence is a closed loop. Nothing solicits ANCS, so the iPhone is never
asked for the service, so it never offers Show Message Notifications, so no LE keys
derive, so the bond is BR/EDR-only -- and the BR/EDR-only reason then told the
reporter to Forget This Device and pair again. The timeline shows them doing exactly
that three times across 4.7 hours, ending each time where they started.

Fixed on our side, since the hardware cannot be:

- `resolve_capability()` only advises a re-pair when one could derive the LE half. With
  no advertising, or no peripheral role, the reason names the adapter as the cause and
  says plainly that re-pairing will not change it. The `bt_pair_result` message follows
  the same rule, keyed on whether the solicitation actually went on air.
- `bt_diagnostics` reports `bluez_version` and `kernel`, and `advertising_manager`
  alongside `advertising_instances`, so "no interface" and "no free instance" are
  separable in the next report. Nothing in the daemon read the BlueZ version before
  this; only `scripts/bt-probe.sh` did.

Also in the dump, unrelated and already reported correctly: MAP cycling through
`no_record` / `busy` / `forbidden` / `none` for hours (another client holding the
phone's single MAP session), and `bt_message_read` with `success: true, synced: 0`
(marked read locally, not pushed to the phone -- working as designed).

### 2026-09-04 - Two HFP hands-free implementations, one UUID

Adding calls meant choosing which stack owns the hands-free profile, and on this
machine both were installed.

- PipeWire 1.6.8's `bluez5` plugin implements HFP HF in `backend-native.c` and, with
  `bluez5.telephony-dbus-service`, publishes `org.pipewire.Telephony` on the session
  bus. It carries the SCO audio, so it can move a call to the desktop speakers.
- BlueZ 5.87 ships its own `profiles/audio/hfp-hf.c` with `org.bluez.Telephony1` and
  `org.bluez.Call1`, gated behind `--experimental` -- which Tether already sets for
  `Bearer.LE1`. It carries no audio.

Both register `org.bluez.Profile1` for `0000111e`. With PipeWire's `hfp_hf` role on:

```
$ busctl --user tree org.pipewire.Telephony
(no ag0)
bluetoothd: profiles/audio/hfp-hf.c:hfp_connect() unable to start connection
bluetoothd: btd_service_connect() hfp profile connect failed: Input/output error
```

PipeWire had registered its profile -- `Registering Profile /Profile/HFPHF
0000111e-...` in `backend-native.c` -- and could serve an *inbound* connection: once
the phone initiated, the log showed `NewConnection ... fd=59, profile /Profile/HFPHF`
followed by `RFCOMM >> AT+BRSF=695`. Only the outbound path was broken, because
`Device1.Connect()` routes into BlueZ's built-in profile, which fails.

Removing `hfp_hf` from `bluez5.roles` and reconnecting resolved it immediately:

```
$ busctl --system introspect org.bluez \
    /org/bluez/hci0/dev_.../telephony0 org.bluez.Telephony1
.State           property s "connected"
.OperatorName    property s "AT&T"
.Signal          property y 1
.BattChg         property y 4
.Service         property b true
```

BlueZ's was taken. It needs no WirePlumber configuration at all, so the machine never
becomes an audio destination and the "Keeping the phone's audio on the phone" advice
stands unchanged; it reuses the ObjectManager subscriptions `BluezMonitor` already
holds; and it reports carrier, signal, roaming and phone battery, which PipeWire's API
does not expose. The cost is that call audio cannot come to the desktop.

The alternative was `bluetoothd --noplugin=hfp` to disable BlueZ's built-in and let
PipeWire own the profile. That buys desktop call audio at the price of a system-wide
change to every Bluetooth device on the machine. Not taken.

Two smaller findings from the same session:

- A backup file left in `~/.config/wireplumber/wireplumber.conf.d/` is loaded as
  configuration if it still ends in `.conf`. Keep backups outside that directory.
- `HangupActive` and `HangupHeld` are in `man org.bluez.Telephony` but are not exported
  by 5.87. Only `HangupAll` is.

### 2026-09-04 - Why the audio cannot follow, and why that is the right trade

BlueZ's hands-free profile signals calls and never opens the voice link. HFP runs over
two connections, RFCOMM for the AT commands and SCO for the audio, and bluetoothd only
uses the first. Its AT vocabulary is complete for control -- `BRSF`, `CIND`, `CMER`,
`CLIP`, `CCWA`, `COPS`, `CLCC`, `CHLD`, `CHUP`, `BLDN`, `+CIEV` -- and contains no
`AT+BCC` or `+BCS`, the Codec Connection commands that are the only way a hands-free
unit asks the phone for audio. `profiles/audio/transport.c`, which implements
`org.bluez.MediaTransport1`, links with `a2dp.c`, `bap.c` and `asha.c` and has no SCO
transport type.

Confirmed on a live call rather than inferred. With a call connected and talking, the
`Call1` object is there and correct, and nothing else is:

```
$ ./scripts/bt-probe.sh --calls
BlueZ telephony objects
  /org/bluez/hci0/dev_<ADDR>/telephony0
      .OperatorName  property s "AT&T"
      .Signal        property y 2
      .State         property s "connected"
      .UUID          property s "0000111f-0000-1000-8000-00805f9b34fb"
  /org/bluez/hci0/dev_<ADDR>/telephony0/call1
      .LineIdentification  property s "<number>"
      .State               property s "active"

Media transports (a transport here would mean the audio can reach the desktop)
  none -- BlueZ exports no transport, so the audio stays on the phone

Audio server view
  no bluez_card -- nothing for the desktop to play through
```

An `active` call with no transport and no audio device is the whole answer.

PipeWire's implementation does carry the audio, and was rejected on maintenance grounds
rather than capability. It needs two prerequisites that both move: PipeWire >= 1.4 for
`org.pipewire.Telephony`, and `bluetoothd --noplugin=hfp` but only on BlueZ >= 5.87. The
required system configuration therefore differs per distro and would appear under
existing users when their distro bumps BlueZ -- working calls breaking on an unrelated
upgrade, with no change in Tether. Ubuntu 24.04, one of this repo's own CI targets and
an LTS supported to 2029, ships a PipeWire predating the API entirely. And it would bind
Tether to one audio server on machines that may run PulseAudio or no audio stack at all.

`org.bluez.Telephony1` costs one prerequisite, `--experimental`, which Tether already
requires, detects and prints the fix for.

It is also the profile-agnostic layer, which is what makes control-only acceptable
rather than a dead end. `profiles/audio/telephony.c` is shared, sitting beside
`profiles/audio/ccp.c` (LE Audio Call Control), and `Telephony1.UUID` exists because more
than one profile can provide it: `0000111f` here over HFP, a TBS UUID on an LE Audio
phone, through the same methods and the same `Call1` objects. LE Audio call audio rides
an ordinary BAP `MediaTransport1` that every audio server already consumes. So the path
to desktop call audio is BlueZ or the phone gaining it, not Tether growing an audio
stack -- and Tether needs no change when it arrives.

Users who want desktop audio today can hand the profile to PipeWire, at the cost of
Tether's call control. Written up under "Getting the call audio onto the desktop
instead", with the mutual exclusivity stated first.

### 2026-09-04 - Walking the PipeWire route, and what it really costs

The instructions above were written from reasoning and then tested before publishing.
Two of the three steps were wrong, which is the argument for testing them.

**`a2dp_sink` is mandatory, and that is the real price.** With
`bluez5.roles = [ a2dp_source hfp_ag hfp_hf bap_source ]` and BlueZ's built-in disabled,
PipeWire registered `/Profile/HFPHF` for `0000111e` -- confirmed on the adapter's UUID
list -- and bluetoothd stopped logging its own `hfp_connect()` failure. Yet no
`bluez_card` was ever created, no `ag0` appeared, and the machine did not show up in the
iPhone's Control Center audio picker. Adding `a2dp_sink` produced the card instantly:

```
$ pactl list cards short
827  bluez_card.60_57_C8_30_6A_F7  module-bluez5-device.c
...
    Active Profile: audio-gateway
```

So iOS decides whether to speak hands-free to a machine based on whether that machine is
an audio destination at all, and A2DP sink is what makes it one. The consequence is that
desktop call audio and "the phone's music stays on the phone" cannot both be had. That
inverts the appeal of the whole route for anyone running the config in "Keeping the
phone's audio on the phone".

**`bluez5.telephony-dbus-service = true` must be set explicitly.** It was left out of the
first draft because `busctl --user status org.pipewire.Telephony` showed the name owned,
which was mistaken for the service being on. Ownership of the bus name is not the same
as the service being enabled; without the setting no gateway is ever registered.

**The desktop cannot bring the link up.** `Device1.Connect()` ignores an externally
registered `Profile1`. `ConnectProfile("0000111e")` fails with `No more profiles to
connect to`, because BlueZ matches `ConnectProfile` against the *remote's* advertised
UUIDs and the iPhone advertises `0000111f`, not `111e`.
`ConnectProfile("0000111f")` returns success and connects nothing meaningful -- BlueZ
matches it to PipeWire's `/Profile/HFPAG`, this machine acting as the gateway, which is
not the wanted direction. Every successful connection observed was initiated by the
phone.

With all three right, it does work end to end:

```
spa.bluez5.native   profile_new_connection: NewConnection ... fd=59, profile /Profile/HFPHF
spa.bluez5.native   RFCOMM >> AT+BRSF=695
spa.bluez5.native   RFCOMM << +BRSF:1007  OK
spa.bluez5.native   RFCOMM >> AT+BAC=1,3,127,2
spa.bluez5.telepho  telephony_ag_register: registered AudioGateway: /org/pipewire/Telephony/ag1
```

One diagnostic worth knowing: `Not activating device bluez_card.<ADDR>` from
WirePlumber's `create-device.lua` is not an error in itself. It gates on
`api.bluez5.connection == "connected"`, which only becomes true once the hands-free
service level connection completes. Seeing it means the AT handshake above has not
finished, or was interrupted -- a short-lived debug instance of `wireplumber` is enough
to cut it off mid-negotiation.

None of this changes the shipped design. It sharpens the reason for it: the supported
path needs one flag Tether already requires, and the alternative needs three settings
across two daemons, gives up call control, gives up keeping the phone's audio on the
phone, and still depends on the phone choosing to connect.

### 2026-09-04 - A Barrot dongle, and a dump that could not say whether it was the cause

Reported as #128. LE never connected across a 1.8-hour timeline while MAP and PBAP
worked throughout, on a machine that passes every capability check Tether makes.

| | |
|---|---|
| Controller | **Barrot Technology Co.,Ltd.** — `manufacturer 2279` (SIG company id `0x08E7`) in the reporter's `btmgmt info`. USB dongle; exact USB id not captured |
| Kernel | 7.0.0-31-generic (Kubuntu 26.04.1) |
| BlueZ | 5.87, self-built, running with `--experimental` |
| Adapter roles | central + peripheral; 3 advertising instances; `secure-conn`, `ll-privacy` on |
| Adapter class | `0x7c0408` — A/V Hands-Free |
| Phone | iPhone 17, iOS 26.6.1 |
| Tether | 0.2.24 |

Captured: `le_connected` false in all 46 timeline entries. `classic_connected` flapped
about 20 times with MAP and PBAP churning `none` / `no_record` / `other` / `forbidden`.
Part of that flapping is the "no locally connectable profile" artifact of 2026-08-23,
but not its rate -- no MT7925 session recorded above looks like this.

Barrot's only presence in the kernel is `btusb.c`'s CSR-clone workaround, whose comment
names "a Barrot 8041a02" among controllers that are "really messed-up", plus
`BTUSB_BARROT` for `33fa:0010` and `33fa:0012`. That is circumstantial: the reporter's
chip is HCI version 13 (Core 5.4), not the BT 4.0 clone the quirk covers. It is the same
shape as #69, where an unbranded CSR clone reported `class=ok`, `secure-connections=on`
and both LE roles and still produced only BR/EDR bonds.

**The finding is that the report could not settle it, and two of our own defects are
why.** Both are #118's lesson again: a precondition reported as established when it was
only assumed, and advice that depends on it.

- **`bond_has_le` did not mean the bond had an LE half.** `resolve_capability()` set it
  from `has_le_bearer` alone, which is only "`Bearer.LE1` carries properties" -- so
  `--bt-status` printed `Bond: BR/EDR + LE` for every bonded device on any machine
  running `bluetoothd --experimental`. `pairing.cpp` had the right predicate all along
  (`has_le_bearer && le_bonded`); the capability path did not. The one question that
  separates "this chip cannot derive the LE LTK", which is #69's story, from "it derives
  keys and never carries the link" is exactly what that field was for, and #128's
  `bond_has_le: true` is worth nothing. Fixed, with a test for a **populated**
  `Bearer.LE1` whose `Bonded` is false -- the existing tests only covered the absent and
  empty-interface cases, which is how it survived.

- **Nothing separated "we registered an advert" from "an advert is on air."**
  `ancs_soliciting` is `AncsAdvertisement::active()`, our own registration flag, and
  `resolve_capability()` read `SupportedInstances` and never `ActiveInstances`. A
  controller that accepts `RegisterAdvertisement` and then does not radiate -- the
  plausible failure for a single-radio chip scheduling an ACL, page scan and inquiry
  scan at once -- is indistinguishable from a phone ignoring a healthy advert. The
  supervisor took the second reading unconditionally and told the reporter their iPhone
  was wedged; they cycled its Bluetooth "countless times". `advertising_active_instances`
  is now reported, and `BearerOps::solicitation_on_air()` gates the advice: a long LE
  silence in which the solicitation was never once observed on air names the adapter and
  says plainly that nothing on the iPhone will change it. The flag is latched across the
  window, because the advert is legitimately off air while a dial owns the radio and an
  instantaneous read there would blame a working adapter.

Also corrected here: **`Adapter1.Modalias` is not the controller.** It is BlueZ's own
device id -- the reporter's `usb:v1D6Bp0246d0557` is Linux Foundation / BlueZ with
`0x0557` encoding version 5.87, and it reads identically on every machine. The chip comes
from `/sys/class/bluetooth/<hciN>/device/modalias`, needs no privilege, and is now
reported as `controller` alongside it and printed by `scripts/bt-probe.sh`. Measured on
the MT7925 machine, the two read `usb:v0E8Dp0717d0100...` and `usb:v1D6Bp0246d0557`
respectively.

**The coexistence question is open.** Nothing here proves the Barrot cannot advertise
while holding a BR/EDR link; it makes the next report able to. What would settle it, in
order of cost: `btmon` showing whether `LE Set Extended Advertising Enable` ever returns
a non-zero status and whether any `LE Connection Complete` occurs at all; an external LE
scanner looking for the `Tether` advert with the iPhone connected and again with its
Bluetooth off; and `tether --bt-adapter <hciN>` onto any other controller. A secondary
suspect worth ruling out is `ll-privacy`, since answering the solicitation means the
iPhone connects with a rotating address the local resolving list has to handle, and clone
firmware gets resolving lists wrong.

No known-bad-controller list was added. One report is not a rule, and naming the hardware
in every dump has to come first.

### 2026-09-05 - The setup command the AppImage prints could not be pasted

Reported as #136 from CachyOS: `sudo systemctl enable --now tether-btclass@hci0` answers `Unit
tether-btclass@hci0.service does not exist`, so iOS never offers Messages and Contacts.

Correct as far as it goes -- an AppImage installs nothing system-wide, and only the distro
packages put `tether-btclass@.service` in `/usr/lib/systemd/system`. That case was already
handled: `CMakeLists.txt` compiles the unit text into the binary and `set_class_command()`
probes the four unit directories, emitting a `sudo tee ... <<'EOF'` here-document when the file
is absent. The path existed and did not work.

`print_bt_setup()` indented every line of a step's command by five spaces so a multi-line
command would read as one block. A here-document delimiter only ends the document at column 0
-- `<<-` strips tabs and never spaces -- so the printed `     EOF` closed nothing:

```
$ bash step.sh
warning: here-document at line 1 delimited by end-of-file (wanted `EOF')
```

Pasted into a terminal the shell sits on a `>` continuation prompt, and the `daemon-reload` and
`enable` lines that follow are swallowed into the unit body instead of running. The GTK Devices
page was never affected: its "Copy commands" button copies the raw string and does no
indentation.

The docs made it worse by never mentioning the split. `docs/BLUETOOTH.md` printed the
packaged-install command as *the* answer and contained no occurrence of "AppImage", "Flatpak" or
"portable"; the README's AppImage section did not mention the class unit at all. So a portable
user following the documentation reached a command that cannot work, with nothing pointing at
`--bt-setup`.

Fixed by printing command lines flush left, which removes the failure mode rather than working
around it, and by adding `tether --install-btclass-unit`: it writes the embedded unit to
`/etc/systemd/system/tether-btclass@.service`, refuses without root, and never enables anything,
so the AppImage now prints three short lines instead of a 24-line paste. Flatpak keeps the
here-document, because `flatpak run` under `sudo` is the wrong user. The test on
`set_class_command()` now asserts the unit text ends in a newline and that the here-document
carries a bare `EOF` line to close it.
