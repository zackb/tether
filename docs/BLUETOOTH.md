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
| AirPods battery, listening mode, in-ear | **AAP** (Apple Accessory Protocol) | BR/EDR / L2CAP | L2CAP client |

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

**Without systemd** (Artix, Void, Devuan) there is no unit to enable and no drop-in
to write, so `--bt-setup` prints edits to `/etc/bluetooth/main.conf` instead:
`Experimental = true`, the same switch as `--experimental`, and `Class = 0x000408`,
applied to the running adapter with `btmgmt`. Restart `bluetoothd` with the machine's
own service manager. The class setting only holds where nothing overrides it, which is
why it is not the systemd answer, see 2026-09-10 below.

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

### Hands-free after a phone-initiated reconnect

BlueZ connects its hands-free profile only on a fresh BR/EDR connect. When the phone
reconnects on its own -- after a Bluetooth toggle, or walking back into range -- it
brings the other profiles up without hands-free, and nothing retries. Measured
2026-09-06, with the phone reconnecting after a Bluetooth toggle:

```
bluetoothd: Device is already marked as connected
bluetoothd: .../sep3/fd0: fd(41) ready
```

No `telephony0`, and all three re-connect paths are dead ends on a live ACL:
`Device1.Connect()` returns success and does nothing, `ConnectProfile("0000111e")`
fails `No more profiles to connect to` (BlueZ matches the remote's UUID list, and the
iPhone advertises `0000111f`), `ConnectProfile("0000111f")` returns success and does
nothing.

Dropping the BR/EDR bearer alone is what recovers it, and it leaves LE and the ANCS
subscription untouched:

```bash
busctl --system call org.bluez /org/bluez/hciN/dev_BDADDR org.bluez.Bearer.BREDR1 Disconnect
```

`BearerSupervisor` does this once per hands-free outage, `HFP_ABSENT_SECONDS` after
BR/EDR comes up without a telephony object, and only while call control is enabled --
the cycle costs the MAP and PBAP sessions riding on that bearer. The flag survives
`reset()`, because the drop the cycle causes runs straight back through it, and clears
only when a telephony object is seen, so a phone that never offers hands-free is asked
exactly once per daemon.

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

So no WirePlumber configuration is needed for the audio to stay on the phone, and
"Keeping the phone's audio on the phone" below continues to apply unchanged. One is
needed for call control, though: stock `bluez5.roles` **includes** `hfp_hf`, a
phone-initiated link then goes to PipeWire, and BlueZ exports no `org.bluez.Telephony1`
at all -- `Calls (HFP): no`, Calls page empty, with nothing wrong on the phone.
Measured 2026-09-06 on wireplumber 0.5.17. Dropping that one role is enough, and it
does not require giving up the phone's music on the desktop:

```
monitor.bluez.properties = {
 bluez5.roles = [ a2dp_sink a2dp_source bap_sink bap_source asha_sink hfp_ag ]
}
```

That gave `telephony0`, a `Calls (HFP): yes` line carrying carrier, signal and battery,
and the iPhone's music still arriving as A2DP AAC at 44100 Hz stereo. Dropping
`a2dp_sink` too is what "Keeping the phone's audio on the phone" describes, and also
leaves call control working.

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

The two cannot own the profile at the same time. Both register for UUID `0000111e`, and
which one gets the link depends on who opens it. Tether reads both, so which one wins
decides where the call audio plays, not whether call control works -- see "Either stack
can serve the calls" below.

A connect from this side routes into BlueZ's built-in profile and fails there:

```
profiles/audio/hfp-hf.c:hfp_connect() unable to start connection
btd_service_connect() hfp profile connect failed for <phone>: Input/output error
```

**A connect the phone opens goes to PipeWire**, measured 2026-09-06 on bluez 5.87 with
`bluetoothd --experimental` and the hfp plugin loaded -- no `--noplugin=hfp` anywhere.
PipeWire's `/Profile/HFPHF` ran the service level connection, `spa.bluez5.native` logged
`rfcomm_hfp_hf: AG indicator state: service = 1`, a live call arrived on
`bluez_input.<addr>` as `api.bluez5.profile = "headset-audio-gateway"` at 24000 Hz mono,
and BlueZ exported no telephony object at all. So the earlier claim that the built-in
profile always wins holds only for the direction this side initiates, and an iPhone
reconnecting on its own is the other direction.

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

### Either stack can serve the calls

`TelephonyClient` holds a list of `TelephonySource`s and uses the first one serving the
phone: BlueZ, then PipeWire. So the machine keeps call control whichever stack ends up
with `0000111e`, and the four objections above stop applying to Tether:

- No new dependency. PipeWire is reached over its D-Bus API on the session bus, and
  `gio-2.0` was already linked. Nothing changes in the package or the build.
- No new requirement. A machine with no PipeWire finds no bus name, the source reports
  nothing, and calls read exactly as they did before -- unavailable, with the same
  sentence. A working BlueZ setup never touches the session bus at all, because BlueZ is
  consulted first and short-circuits.
- CI is unaffected: the parser is a pure function over a recorded `GetManagedObjects`
  payload, tested with no bus, like the BlueZ one beside it.

What differs between the two sources is what they can report and carry:

| | BlueZ | PipeWire |
|---|---|---|
| Call control | yes | yes |
| Call audio | never | on this computer |
| Carrier, signal, battery, roaming | yes | none of them |

PipeWire's `AudioGateway1` carries no HFP indicators at all, so the payload marks them
absent (`indicators: false`) rather than reporting their defaults, and both the CLI and
the Calls page drop that line instead of claiming "No service" for a phone that has
service. `--bt-call-audio on` calls `AudioGatewayTransport1.Activate()`; `off` sets
`RejectSCO`, which gates the next voice link rather than tearing down one already up.

### Getting the call audio onto the desktop instead

Possible, and it costs more than it first appears. Walked end to end on 2026-09-04, so
what follows is measured rather than reasoned.

You give up two things:

- **The HFP indicators.** Handing the profile to PipeWire means BlueZ no longer owns it
  and exports no `org.bluez.Telephony1`. Call control itself survives -- Tether reads
  PipeWire's API too -- but carrier, signal strength, battery and roaming have no
  PipeWire equivalent and stop being reported.
- **The phone's audio staying on the phone.** `a2dp_sink` turns out to be mandatory
  here, so music and system sounds move to the desktop as well. Everything
  "Keeping the phone's audio on the phone" below is written to avoid, you are opting
  back into. This is the part that surprises people, so it is stated first.

Tether has no code on this path and does not test it in CI.

Step 3 is the one that matters. Steps 1 and 2 were both required in the 2026-09-04 walk
and neither was on 2026-09-06 with pipewire 1.6.8 / wireplumber 0.5.17 / bluez 5.87:
stock `bluez5.roles` already carries `hfp_hf` and `a2dp_sink`, so with no wireplumber
configuration at all and the hfp plugin still loaded, a phone-initiated link put call
audio on the desktop on its own. Treat steps 1 and 2 as what to reach for when the
defaults do not do it, and check the versions before assuming they are needed.

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

## AirPods

Battery for the buds and the case, the listening mode, and in-ear detection, in the
Devices list and from `tether --bt-airpods`. A single stem press plays and pauses here.

**All of it is off until switched on** -- `tether --bt-airpods-enable on`. See "Standing
down" below.

It is the one feature here that does not go through BlueZ. AirPods report battery over
Apple's Accessory Protocol on an L2CAP channel, PSM `0x1001`, the same source an iPhone
uses. `src/core/src/bluetooth/airpods.cpp` opens it directly and is the only raw
Bluetooth socket in the tree; the kernel's L2CAP constants and address struct are
declared there rather than pulled in from bluez-libs, which the rest of the build does
not need. No root and no capabilities: PSM `0x1001` is above the privileged range.

BlueZ is still what says *which* device to open: `Device1.Modalias` starting
`bluetooth:v004c` plus an A2DP sink UUID, falling back to a name containing "airpod"
because Modalias is absent until SDP has been read once. The name alone is not enough —
AirPods can be renamed to anything — and the vendor id alone is not either, since an
iPhone is the same vendor.

### The wire protocol

Three packets on connect, in order, then read. All three are required: without
set-features, or with anything but the five-`FF` request, no notification ever arrives.

```
handshake              00000400010002000000000000000000
set-features           040004004d00d700000000000000
request-notifications  040004000f00ffffffffff
```

They are **paced, not sent in one burst**. The handshake goes 200ms after the channel
opens, set-features on the handshake acknowledgement, the notification request on the
features acknowledgement or 600ms in, whichever comes first. The firmware discards
anything sent ahead of its own pace, silently: no error, no refusal, the packet simply has
no effect. Each step also has a deadline, because some models never acknowledge and an
unacknowledged step must not stall the session.

Two more notifications are read for ownership:

```
connected-devices  04 00 04 00 2E 00 [?] [?] [count] ([mac] * 6 [role] [state]) * count
audio-source       04 00 04 00 0E 00 [mac] * 6 [type]
```

`state` carries ownership in **bit `0x02`**, and nothing here should be compared for equality.
Measured 2026-09-11 across five handoffs, each one confirmed by the firmware's own verdict packet
arriving in the same instant:

| host | idle | owning |
|---|---|---|
| this machine | `0x00` | `0x02` |
| the iPhone, AirPods Pro 3 | `0x05` | `0x07` |
| the models the AC reference was written against | `0x15` | `0x17` |

Bit `0x10` is separate: it marks a host that firmware treats as engaged, and a claim sent into
that range leaves this host contested. So a peer blocks a claim when it sits at `0x10` or above,
does not carry the owner bit, and is not `0x15` -- **owning does not block one**, because an owner
that has finished its call is exactly what a reclaim takes the buds from, and an iPhone owning at
`0x17` stays there until another host claims. The iPhone keeps the owner bit until another host
claims, so ownership alone never means "still using them" either; that needs the audio source.

**The owner bit is corroboration, not the decision.** It is dependable on some setups and not on
others, and the difference is not visible from here. Measured 2026-09-15: on one machine the phone
moves `0x05` idle to `0x07` owning on every handoff, exactly as the table says; in the log attached
to #193 the same iOS version reports `0x05` while the audio-source notification names that phone as
the host the buds are carrying media for -- either it never took ownership, or its host list was
never refreshed, and the two are indistinguishable in a log that only records changes. What the buds
carry, and for whom, is the audio-source notification, and that is what `peer_busy()` reads.

The AC reference reads the low values as validation state instead (`0x00` validated,
`0x01` pending, `0x02` unvalidated, `0x03` both, `0x05` a stuck session) and says ownership is not
in this field. That does not match this hardware: our own entry alternates `0x00` and `0x02` in
lockstep with the verdict packet, which would mean flapping between validated and unvalidated on
every handoff. Both readings agree that `0x10` and up is engagement.
`0x15` is carved out because it is what a **connected, idle iPhone** reports, permanently, and
it is the state this machine claims the buds *out of*. Treating it as "a peer has them" means
the claim is never sent and the phone keeps the buds -- which is exactly what a first cut of
this did.

The address is in **display order in connected-devices and least significant byte first in
audio-source**. Getting it wrong makes a machine read its own entry as a peer. Both parsers
return BlueZ's own text order.

`type` in audio-source is a hint and nothing more: iOS reports `MEDIA` for real calls, and
sends `NONE` every twenty seconds or so *during* one. Deciding anything from it alone
takes the buds off a live call.

A battery notification is `04 00 04 00 04 00 [count]` followed by `count` five-byte
entries, `[component] 01 [level] [status] 01`. Components are Right `0x02`, Left `0x04`,
Case `0x08`. Status is a **bitmask**: `0x04` is not reporting -- a bud in the case, a shut
case -- and it combines with the charging bits, so a pod charging in an open case reports
`0x05`. Testing for equality misses exactly the common case. Not reporting is shown as
unknown, never as 0%. A notification carries only what changed, so anything it leaves out
keeps its previous level.

### Stem press

```
stem config  04 00 04 00 09 00 39 [mask] 00 00 00     single 01, double 02, triple 04, long 08
stem press   04 00 04 00 19 00 [type] [bud]           type 05 single .. 08 long, bud 01 left 02 right
```

Without the stem config a single press runs the firmware's own play/pause, and **that is what moves
the audio to the iPhone**: the Apple toggle between hosts. With mask `01` the buds send the press
here instead and do nothing themselves, so Tether toggles MPRIS (`MediaControl::toggle()`). Double,
triple and long keep their firmware actions. Found by AirPods Center (orychalk's fork), validated
there on Pro 2 and Pro 3, and here on Pro 3.

It is dropped silently until the state broadcast the notification request starts has finished, so
it goes out 1.5s after the channel opens (`STEM_CONFIG_AT_MS`), and again with the notification
request after every claim, because the firmware resets both on each handoff.

### Smart routing

Hosts sharing the buds talk to each other through them. A host sends opcode `0x10` addressed to
another host; the buds deliver it as `0x11` carrying the sender:

```
04 00 04 00 10 00 [target] * 6 [length] * 2 01 [OPACK dictionary]     sent
04 00 04 00 11 00 [sender] * 6 [length] * 2 01 [OPACK dictionary]     received
```

Addresses least significant byte first; `length` little-endian, counting from the `01`. The body is
Apple's OPACK: `E0+n` a dictionary of n entries, `40+len` a short string, `08+v` a small integer,
`30`/`31`/`32` an 8/16/32-bit little-endian integer, `01`/`02` true/false, `A0+i` a back-reference
to the i-th distinct string or wide integer so far. `parse_smart_routing()` decodes that subset and
`smart_routing_packet()` encodes it. LibrePods builds these packets by hand and gets several wrong
(`42 "YES"`, a missing tag before `btName`), so nothing here is copied byte for byte.

What an iPhone sends, measured 2026-09-13:

| message | keys |
|---|---|
| media report, on every change | `playingApp`, `hostStreamingState` `YES`/`NO`, `btAddress`, `btName`, `otherDeviceAudioCategory` |
| idle heartbeat | `idleTime`, `btAddress`, `btName`, `nearbyAudioScore`, and `newTipi` on first contact |
| yield request | `audioRoutingSetOwnershipToFalse` true, `reason` (`ManualRoute`) |

`otherDeviceAudioCategory` is `100` for nothing, `301` for media (`com.apple.Music`) and `501` for a
call (`com.apple.TelephonyUtilities`). `YES` with `100` is a route with nothing on it -- Control
Center, or the moment a call hangs up -- so playing is `YES` with a category above 100.

This machine sends the same media report, `tipi_add_device()` when it first sees a host, and on a
take-over `smart_routing_hijack()`: `localscore`, `reason` `Hijackv2`, `audioRoutingScore`,
`audioRoutingSetOwnershipToFalse` and `remotescore`, LibrePods' values with its references resolved.
`btName` is `Android`, as both references send.

### Listening mode

Off, Transparency, Adaptive and Noise Cancellation, set from the AirPods page in the GTK
app or with `tether --bt-airpods-mode <off|anc|transparency|adaptive>`.

Sent and received on the same 11-byte shape, `04 00 04 00 09 00 0D [mode] 00 00 00`, with
the mode at offset 7. The wire values are one above the order the modes are usually listed
in: `01` Off, `02` Noise Cancellation, `03` Transparency, `04` Adaptive.

**A mode the buds decline is simply not applied, and they say nothing about it.** Measured
2026-09-08 on AirPods Pro: `off` was sent three times and the buds stayed in the previous
mode, while `anc`, `transparency` and `adaptive` all took effect within a second. Apple
leaves Off out of the noise-control rotation by default on Pro models, and there is no
error and no refusal packet -- the only evidence is that the mode notification never
changes.

So nothing here assumes a write succeeded. The published mode only ever comes from the
buds' own notification, which they send unprompted on every change including the ones the
desktop asked for. The GTK buttons snap back to the real mode when a write does not take,
and a model with no listening mode at all reports none and the buttons stay disabled.

Writes are queued for the reader thread rather than sent from the caller's, because the
socket belongs to the session and closing it out from under a write is the one race worth
not having. A queued change does not outlive the channel it was meant for.

### In-ear detection, and pausing

Eight bytes, `04 00 04 00 06 00 [primary] [secondary]`, with `00` in the ear, `01` out of
it, `02` in the case and anything else unknown.

They are a **primary and a secondary, not a left and a right**, and which bud is which
moves between them. So nothing here names a side; the count of buds in ears is what the
policy and the UI use.

Taking a bud out can pause whatever is playing locally, over MPRIS. It is **off by
default** -- `airpods_pause` in `bluetooth.json`, `never`, `one-removed` or `both-removed`
-- for the same reason call control is: nothing reaches out and touches the user's session
until they ask for it. `tether --bt-airpods-pause one-removed`, or the dropdown on the
AirPods page.

The policy is `ear_media_action()`, kept pure so its awkward cases are settled by tests
rather than by taking earbuds out repeatedly:

- **The first ear report after connecting is not a transition.** The buds say where they
  already are as soon as the channel opens, and reading that as a change would pause the
  music the moment a case is opened near the machine.
- **Only a pause Tether made is undone.** Playback the user stopped by hand stays stopped.
- A bud in the case counts the same as a bud out of the ear.
- **Buds that disconnect entirely leave the pause in place.** Resuming there would move
  the audio to the machine's own speakers, which is not what taking an earbud out asked
  for.

MPRIS itself is `src/core/src/mpris.cpp`, a plain session-bus client: `ListNames`, keep
the `org.mpris.MediaPlayer2.` ones, pause the players reporting `Playing`, remember them,
play those back. Calls carry `G_DBUS_CALL_FLAGS_NO_AUTO_START` and a one-second timeout,
because a bus name can be *activatable* rather than running -- `playerctld` usually is --
and neither starting a media player to interrogate it nor blocking the Bluetooth reader
thread on one is acceptable.

### Handing the buds to the phone for a call

Off by default. `tether --bt-airpods-handoff on`, or the checkbox on the AirPods page.

There are two ways to do it, and which one runs is decided by the machine, not by a
setting: **ownership handoff** when the adapter presents itself as Apple hardware, and
**disconnect handoff** when it does not. `apple_device_id` in `bt_status` says which, and
the AirPods page prints the same thing in a sentence.

#### Ownership handoff

Apple's own. The buds keep both hosts connected and hand *ownership* between them, so the
link to this machine is never dropped, the AAP channel is never given up, and the volume
the stem sets is the volume of whoever owns them.

**It runs on what the other hosts report through the buds**, the smart-routing media reports
above. No timer decides anything, and it does not need call control.

- **The phone gives the buds up once it is actually using them**, call or media alike: it reports
  itself playing *and* the buds report their audio going to it (`AirPodsState::peer_busy()`,
  `peer_holds_the_audio()`). Neither half decides alone -- an iPhone playing on its own speaker
  reports the same media, and the owner bit answers nothing, having been seen idle on a phone the
  buds were carrying media for.
- **A call report is not a yield.** Announcing a call is not taking the buds. Tether held a version
  that released on category `501` before the phone claimed; measured 2026-09-15 over four calls on
  one iPhone, the two where Tether released first were the two where the buds never carried the
  call -- no audio-source notification for the phone at all -- while the two where the phone claimed
  first (`audioRoutingSetOwnershipToFalse`, owner bit to `0x07`) routed within a second. Releasing
  early hands the phone a device whose owner has just vanished and whose transport is down. The
  call report still holds *this* machine's claims back (`takes_over_for_play()`), and losing the
  owner bit still pauses local playback on the spot, so nothing reaches the speakers while the
  phone takes over.
- **The phone reporting `NO` takes them back**, and resumes what the release paused.
- **A player starting here takes them**, wherever they are, unless the phone is on a call. MPRIS
  `PropertiesChanged` drives it (`MediaControl::watch()`). With the card down the player is paused
  on the signal, the card comes back, and it resumes -- the pause is what keeps it off the
  speakers. The claim goes with a media report saying something plays here and a hijack request,
  which the iPhone answers with `NO`. Players that leave `PlaybackStatus` empty (Chromium without
  Plasma Browser Integration) are not seen.
- **A yield request from another host** releases at once, without waiting for the verdict.
- **Losing the owner bit pauses local playback** on the spot, whoever took it.
- Every change in whether something plays here goes to the other hosts (`set_streaming()`).

With `a2dp_sink` in `bluez5.roles` the iPhone falls back to this computer as a speaker when it loses
the AirPods here, instead of staying on the buds; see "Keeping the phone's audio on the phone".

Releasing pauses first, then waits for the sink to go quiet, sends the release and switches the
card off; see "The AirPods sink jammed after a call" for why the card, not the default sink.

**Both buds out moves the stem and volume target to another host.** A session that owned the
buds when the last one came out claims again when one goes back in (reported by orychalk).

`ownership_action()` and `takes_over_for_play()` are pure and tested. Buds already given up come
back even if the setting was switched off while they were away.

Kept from before, because they are firmware facts rather than guesses:

- **Never claim while a peer is taking the buds.** A claim sent then leaves this host
  contested, and the firmware closes the link about twenty seconds later at the next
  physical event. An idle peer at `0x15` and an owner are not taking them, per the wire protocol.
- **The session claims once when it opens**, five seconds in (`CLAIM_SETTLE_MS`): AirPods Pro 2
  validates a host by itself, Pro 3 never does, and an unvalidated host is dropped at the first pod
  movement. Three seconds later (`IDLE_RELEASE_MS`, `releases_when_idle()`) ownership goes back
  unless the buds have reported audio for this machine, so a machine that is not using the buds
  does not hide them from the phone. A watcher that has given the buds up sends no claim of its
  own.
- **Every other host in the buds' list is registered, once per session**, with a media report
  and `tipi_add_device()`.
- **The notification request and stem config go out again 500ms after every claim**: the firmware
  resets its AAP state on each handoff and silently drops what a session set up before it.

#### Disconnect handoff

The fallback. When the iPhone has a call and the AirPods are connected to *this machine*,
Tether pauses local playback and disconnects them, so the phone can take them. When the
call ends it reconnects them and resumes what it paused.

**It needs call control**, because the trigger is the iPhone's call state, which comes
from `org.bluez.Telephony1` or `org.pipewire.Telephony`. With calls off there is no
gateway and nothing to trigger on, so the checkbox is disabled. See "Calls".

`handoff_action()` is pure and tested. What it refuses to do matters more than what it
does:

- **Only buds this code disconnected are reconnected.** Buds the user took away by hand
  stay away.
- A call while the buds are already on the phone does nothing: there is nothing here to
  hand over.
- It never releases twice, which would lose track of what to give back.
- Playback resumes **only if the buds came back**. Resuming onto the machine's own
  speakers is not what handing them over asked for.

Handoff and "pause when a bud comes out" both act on the same paused players, and they
disagree about what a disconnect means. Buds that vanish on their own leave the pause in
place, because resuming would move the audio to the machine's speakers -- but buds handed
to the phone are coming back, and their pause has to survive until they do. So the ear
watcher only drops the pause when handoff is not the one holding them, and handoff claims
them *before* the disconnect rather than after: the buds go away between those two points,
and the watcher runs in that gap. Both halves were found on a live call, not in review.

**Handing the buds over gives up the AAP channel, and something else may take it.**
Observed 2026-09-08: after a call, the buds came back and playback resumed, but battery
and listening mode stayed empty and the status went to `busy` -- a competing AAP client on
the same machine had claimed the single-client channel during the window when Tether had
disconnected them. Nothing is wrong with the buds or the handoff; it is the conflict
described above, made more likely by the disconnect. Tether keeps retrying and gets the
channel back whenever the other program lets go.

Reclaim is bounded: a settle delay, then three attempts, then it gives up and forgets
them. The phone does not drop the buds the instant a call ends, and retrying forever
would fight it for them. Releasing is synchronous because the phone is ringing now;
reclaiming runs on its own thread because it has to wait.

#### Getting the audio back on the buds, not the speakers

Both paths have the same trap at the end. The buds come back on the link before their sink
comes back in the audio server, and playback resumed in that gap plays out of the
machine's own speakers -- which is what the user hears, and it was reported as a bug.

So a reclaim waits for the sink. `src/core/src/audio.cpp` asks `pactl`, which both
PulseAudio and PipeWire answer, for `bluez_output.<address>` and makes it the default sink
again before anything is resumed, up to eight seconds.

It only does that when the buds *were* the default sink at the moment they were given up.
Whether the A2DP profile is active is the wrong question -- it is active whenever the buds
are connected, even while the user listens on speakers -- and so is looking for streams on
the Bluetooth sink, because a virtual sink for an equaliser takes them instead. The
default sink is the one thing that reflects what the user actually chose.

On the ownership path the sink also has to be **rebuilt**. The phone tears this machine's
A2DP transport down when it takes the buds, and PipeWire never reopens a sink left sitting on
it: playback resumes into silence until the buds are reconnected. So giving the buds up pauses
the players, reads the sink's raw volume, and switches the card profile `off` (A2DP profiles
only) before releasing. Taking them back claims, switches the card back to the saved profile and
reads it back (`pactl` can report success while the card stays off), waits for the **new** sink
-- it has a new index -- and restores the volume before anything resumes. If the phone has
taken the buds again by then, the card goes back off and nothing resumes. A call can also drop
the link outright and the buds reconnect with the card already back on, so there was nothing to
switch off on release; the reclaim then switches it off first. See 2026-09-11 below.

ponytail: no silence stream. A host that is itself the audio source has to write inaudible
silence to keep the sink from being torn down while idle, and to prime the encoder after a
rebuild; here the media player is the source and the sink wait covers the gap. Add one if
crackling is heard after a reclaim.

**What it does not cover.** On the disconnect path, music or video playing on the iPhone
is not a call, so nothing reacts to it. Seeing that would mean the machine acting as an
A2DP sink for the phone and reading `org.bluez.MediaPlayer1`, which is the opposite of
"Keeping the phone's audio on the phone" below. Not attempted. The ownership path gets it
for free, because the buds report the phone as active either way.

Neither path covers a call **dialled from this machine** over HFP. iOS routes it to the
hands-free unit that placed it, the buds never see the phone become active, and no
host-side action changes that once the call has started. Pick the AirPods on the phone.

The constants come from librepods `linux/airpods_packets.h`. The prose
`docs/AAP Definitions.md` in the same repository disagrees with the header and is wrong.

### Presenting as Apple hardware

`DeviceID = bluetooth:004C:0000:0000` under `[General]` in `/etc/bluetooth/main.conf`, then
`systemctl restart bluetooth`, makes the machine present itself to the buds as Apple hardware.
`Adapter1.Modalias` reports `bluetooth:v004Cp0000d0000` once it has taken, which is what
`Adapter::presents_as_apple()` reads and what `apple_device_id` in `bt_status` publishes.

Reading battery does not need it, and neither does setting the listening mode -- confirmed
2026-09-08 with the setting absent from `main.conf` on the test machine. **Apple's own handoff
does need it**: without it the buds offer no AAP ownership to this machine and the only way to
give them to the phone is to disconnect them.

It is a global adapter setting, not a per-application one, and it is not free. Once the firmware
believes it is talking to an Apple device it applies Apple expectations: the session watchdogs, the
ownership arbitration, and an intolerance of rapid repeated card-profile changes. A generic headset
connection is more forgiving precisely because the firmware expects nothing of it.

Tether never writes the file. It reads the result and says what is missing.

### Standing down

`airpods_enabled` in `bluetooth.json`, **off by default**. `tether --bt-airpods-enable on`, or the
checkbox at the top of the AirPods page. While it is off Tether opens no AAP channel at all: no
battery, no listening mode, no in-ear pausing, no handoff, and nothing to fight another AirPods
program for. It is a setting rather than a race to be won: see below.

Off by default for the same reason call control and pause-on-removal are. The channel takes one
client per machine, so opening it takes the buds away from whatever the user already runs -- and
that is not a thing to do to somebody who installed Tether for its notifications.

A reclaim outlives it. Buds already handed to the phone come back when the call ends even if the
setting was turned off while they were away, because the alternative is buds stranded on the phone
and local playback paused with nothing left to resume it.

### The channel takes one client

This is the failure worth knowing about. A second client on PSM `0x1001` — and an
established channel that has silently died — makes `connect()` block **indefinitely and
report nothing**. There is no error to log and no timeout of its own, so the naive
symptom is a connected device whose battery never arrives and a daemon that looks idle.

So the socket is non-blocking and every wait has a deadline: 8s to open the channel, 8s
for the first notification, then 5 minutes of silence before the channel is recycled.
Three attempts in a row that reach a deadline having delivered nothing are reported as
`busy` rather than retried quietly, because the remedy is to stop the other program and
nothing in the log would otherwise say so.

Anything else on the machine speaking AAP is such a program: LibrePods, AC,
a status-bar widget that reads battery itself. Only one of them can hold the channel.

## Conflicts

The iPhone serves one MAP session at a time. Any other program on any machine holding it will block Tether.

Only one program per machine can hold an AirPods AAP channel. See "AirPods" above.

## Known limits

These are properties of what iOS exposes:

- MAP reports both SMS and iMessage as `Type: sms-gsm`. The transport a message used is not knowable.
- The iPhone's MAP sent folder is empty and sending produces no useful outgoing event, so sent history cannot be recovered from the phone. Tether records its own sends.
- ANCS supports positive/negative notification actions only. There is no free-text reply over ANCS, replies go through MAP.
- No attachments, reactions, typing indicators, or read receipts.
- MAP gives no conversation identifier and no participant list for group messages, so group support is a guess and is conservative by default.
- No RSSI for a connected device, so proximity is a yes/no, never a distance. See the 2026-09-08 entry below.
- A phone that switches Bluetooth off, goes into airplane mode or runs out of battery reports `Reason.Remote`, which is indistinguishable from the user doing it deliberately. Locking on away therefore does not cover it.

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
| `tether-btclass@hci0` fails with `Invalid Index`, having worked for months | The controller re-enumerated -- a failed firmware handshake resets it over USB and it comes back as `hci1` -- so the instance name points at an adapter that no longer exists | Update the unit -- it resolves the adapter itself now and the instance name is only a hint, see 2026-09-11 below. By hand, `ls /sys/class/bluetooth` names the live adapter |
| `tether --bt-setup` keeps printing the experimental step after the drop-in was applied and `bluetooth` restarted | Before 0.2.35 the check read `/proc`, which a Flatpak sandbox cannot see, and which `main.conf` `Experimental = true` never touches | Update. Confirm the flag itself with `busctl --system introspect org.bluez /org/bluez/hci0 \| grep AdvertisementMonitor` -- a hit means the experimental API is on, see 2026-09-20 below |
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
| The log repeats `RegisterAdvertisement failed: ... AlreadyExists` for minutes after one timeout | A registration whose call timed out is still held by BlueZ, and the local flag said otherwise so nothing released it | Fixed. A timed-out registration is now released and an `AlreadyExists` is adopted rather than discarded -- see 2026-09-07 below |
| Pairing warns `RegisterAdvertisement ... doesn't exist` on `org.bluez.LEAdvertisingManager1` | BlueZ exported no advertising manager on this adapter at all, because the controller reports no LE advertising support. Distinct from the row above, where the interface exists and the call is refused | Restarting `bluetooth.service` changes nothing. Confirm with `./scripts/bt-probe.sh`; the only route to ANCS is a controller that can advertise, selected with `tether --bt-adapter <hciN>` -- see 2026-09-03 below |
| LE never connects and the log repeats `le-connection-abort-by-local` | Something on this side is cancelling the connection. Tether's own cause was a `PreferredBearer` write racing the async connect, fixed; anything else writing that property during a connect will do the same | Check no other Bluetooth tool is driving the same device. The phone is not the cause: `abort-by-local` means the local host cancelled |
| Messages stopped after turning notification mirroring off | Fixed. The toggle used to restart supervision, which abandoned the MAP and PBAP sessions at obexd instead of removing them, and the iPhone serves one MAP session at a time | Nothing. Mirroring is switched in place now, and a dropped profile supervisor releases its sessions. On an older build, restart `tetherd` |
| Notifications stopped and never came back, while messages and contacts kept working | Fixed. The bearer supervisor used to stop retrying LE after six attempts, and only a Classic drop or a daemon restart re-armed it | Nothing. The solicitation is kept on air whenever LE is down, which is what the iPhone answers -- see 2026-08-22 |
| `tether --bt-status` reports `Bond: BR/EDR only` | The bond was made without cross-transport key derivation, so it has no LE half and can never carry ANCS | Forget this computer on the iPhone and pair again -- it can take more than one attempt, the derivation is flaky on identical inputs (2026-08-25). Check `secure-connections` in the same output first: re-pairing cannot help while it is off |
| `tether --bt-status` reports `Bond: BR/EDR, notifications live` | BlueZ is too old for the bearer API (5.72 in #226), so it cannot show an LE half, but ANCS is subscribed and delivering | Nothing. The BR/EDR-only warning is held back while ANCS is live, and comes back while the phone is away |
| Walked back into range and nothing reconnected for minutes | Fixed. The ANCS advert was gated on the Classic link, and the Classic backoff had no event that ended the absence | Nothing. The advert stays on air whenever LE is down, and an LE link coming up clears the Classic backoff -- see 2026-08-22 |
| Startup logs `StartNotify not ready yet (InProgress)` for up to a minute | GATT discovery is still running on the new LE link | Nothing. It subscribes on its own. Only treat it as the 2026-08-19 hang if the LE link never comes up |
| `tether --bt-connection` reports LE and messages up but `Notifications: no`, for hours | Fixed. The LE link was opened by the dial and carries no ANCS. A connected link used to take the solicitation off air, so the phone was never asked for the service | Nothing. The advert goes back on air over a link that has stayed up without ANCS -- see 2026-08-23. To clear it by hand on an older build, `tether --bt-solicit`; do not re-pair, and do not cycle the phone's Bluetooth |
| LE never comes up on a `BR/EDR + LE` bond, the advert is on air, and cycling the phone's Bluetooth changes nothing | The bond is pinned to `PreferredBearer=bredr`, so the inbound LE link the iPhone opens is never accepted | Fixed. The supervisor hands the preference back to `le` once the Classic link has settled, on every bond including older ones. **Re-pairing was never the fix** -- the Classic fallback re-pinned it on the next drop -- see 2026-09-07 below. `tether --bt-diagnostics` reports `preferred_bearer`; by hand it is `busctl set-property org.bluez /org/bluez/hci0/dev_<ADDR> org.bluez.Device1 PreferredBearer s le` |
| `tether --bt-connection` says `LE: yes` and `Notifications: yes`, but nothing arrives, and only toggling Bluetooth on the iPhone fixes it | The LE bearer dropped while Classic stayed up, and BlueZ left `Notifying` set on the ANCS characteristics, so the daemon read the dead link as live and stopped trying to recover it | Fixed. `le_link_up()` now requires `ServicesResolved` alongside `Notifying` -- see 2026-09-07 below. On an older build, toggling the phone's Bluetooth is the only remedy, which is why it was the only one that ever worked |
| Notifications go quiet after a reconnect and dismissing one logs `ATT error: 0xa2`, with `ancs_ready: true` | Fixed. ANCS UIDs are per-connection counters, and the reset for them was bound to the device path -- which the bearer grace window deliberately holds across exactly the reconnect that rotates them | Nothing. The reset now runs when the notification subscription comes up -- see 2026-09-08 below. On an older build, restarting the daemon clears it until the next reconnect |
| The status says the iPhone is not answering on LE, and its permission is on | The phone's Bluetooth stack is wedged, which the granted permission does not prevent | Turn Bluetooth off and back on **on the iPhone**. Re-pairing and re-toggling the permission do not clear this |
| The status says this computer is not putting the notification request on air | The adapter reports LE advertising support and BlueZ is holding no advertising instance for it, so the iPhone is never asked for the service | Nothing on the iPhone, and re-pairing will not help. Check `controller` in `tether --bt-diagnostics` and try another with `tether --bt-adapter <hciN>` -- see 2026-09-04 below |
| Everything connects but `ancs_ready` stays false | Compatibility mode, or iOS has not authorized notification content yet | Check `Mode:` in `tether --bt-status`. In full mode the daemon retries, the first request returns `NotPermitted` until the prompt on the phone is approved |
| A group conversation cannot be replied to | Working as designed until the route is unambiguous | The thread's `reply_reason` says which condition failed |
| The iPhone never offers the "Show Notifications" toggle, and messages and contacts work | Fixed. A BR/EDR-only bond used to latch notification mirroring off in the config, which takes the ANCS solicitation off air -- so the phone is never asked for the service | Nothing. On an older build, `tether --bt-ancs on` then `tether --bt-solicit`; `tether --bt-status` now shows mirroring under `Notifications:` -- see 2026-09-01 |
| Pairing bonds but the LE half never derives, on a machine with a USB dongle plugged in | Tether used the first powered controller, which is the dongle, not the built-in one | `tether --bt-status` marks the controller in use; `tether --bt-adapter <hciN>` picks another -- see 2026-09-01 |
| The link reads down forever with `br-connection-unknown`, while messages, contacts and notifications all work | This computer offers the iPhone no BR/EDR profile to connect to, and BlueZ only reports a link up while some local profile is connected | Nothing. Tether no longer waits on that link -- see 2026-08-23 below. Call support does not change this: BlueZ's hands-free profile is not one of the local profiles BlueZ counts |
| The iPhone's audio moves to the computer when Tether connects | The machine advertises itself as a Bluetooth speaker/headset, and iOS routes to it. Not caused by Tether beyond bringing the link up | See "Keeping the phone's audio on the phone" below |
| After this computer takes the AirPods, the iPhone plays on the computer and needs the AirPods picked in Control Center | `a2dp_sink` makes the computer a speaker for the phone, and iOS falls back to the last one it had | See "Keeping the phone's audio on the phone" below |
| `tether --bt-calls` reports call control off | PipeWire took the profile *and* its telephony D-Bus service is off, the iPhone reconnected on its own and never opened hands-free, or `bluetoothd` is running without `--experimental` | Either give BlueZ the profile by dropping `hfp_hf` from `bluez5.roles`, or set `bluez5.telephony-dbus-service = true` and let Tether drive PipeWire's gateway -- see "Calls". The daemon cycles the BR/EDR bearer once per outage for the second cause; confirm with `busctl --system tree org.bluez \| grep telephony` and `busctl --user tree org.pipewire.Telephony` |
| AirPods are listed but the battery stays blank, and the row says another program is using the channel | The AAP channel takes one client, and something else has it | Stop the other AirPods program (LibrePods, AC, a status-bar widget that reads battery), or hand it over with `tether --bt-airpods-enable off` |
| Setting the AirPods listening mode to `off` does nothing, while the other three work | The buds declined it. Apple leaves Off out of the noise-control rotation by default on Pro models, and there is no refusal to report | Add Off to the rotation on the phone, under Settings > Bluetooth > (i) > Noise Control. Tether keeps showing the mode the buds are actually in |
| The AirPods handoff checkbox is greyed out | Neither path is available: the adapter does not present itself as Apple hardware, so handoff would have to trigger on the iPhone's call state, and call control is off | Set the adapter Device ID for ownership handoff, or turn on calls: `tether --bt-calls-enable on` |
| A call started but the AirPods stayed on this computer | They were not connected here when it started, handoff is off, or the call was dialled from this computer | Nothing to do in the first and last case -- pick the AirPods on the phone. Otherwise `tether --bt-airpods-handoff on` |
| Music resumed on the computer's speakers after a call | Fixed. A reclaim now waits for the buds' sink to come back before resuming | Nothing. `pactl` must be on `PATH` for the wait to work |
| A call dialled from this computer is routed to it by the iPhone and nobody hears anything | Stock `bluez5.roles` carries `hfp_hf`, so PipeWire owns the profile and rejects the SCO by default (`bluez5.telephony.default-reject-sco`). iOS routes a call to the unit that dialled it and does not reconsider, so the audio is offered to a machine that is refusing it | `tether --bt-call-audio on` accepts it for the current call. To keep call audio on the phone, where the AirPods are, drop `hfp_hf` per "Either stack can serve the calls" and reconnect |
| `--bt-call-audio on` answers `InvalidState` | `Activate` applies to audio the phone is offering right now, and there is none pending | Nothing. The `RejectSCO` half is applied either way, so the next offer is accepted |
| The stem swipe changes the iPhone's volume, not this computer's | The buds send volume to whichever host owns them, and only ownership handoff makes that this machine. Up to 0.2.31, buds that report an owning iPhone as `0x17` were never claimed at all | Set the adapter Device ID, per "Presenting as Apple hardware". On `0x17` models, update; see 2026-09-12 below |
| The AirPods came back after a call but playback did not resume | Fixed. Disconnecting the buds for the call used to clear the remembered players, so there was nothing left to resume | Nothing. Press play on an older build |
| The AirPods did not come back after a call | The phone still had them after three attempts, so Tether gave up rather than fight it | Reconnect them from the phone or the Devices list. Playback is deliberately left paused |
| Playback does not pause when a bud comes out | Pause on removal is off, which is the default | Set it on the AirPods page, or `tether --bt-airpods-pause one-removed` |
| Playback pauses on removal but never resumes | Something else paused or stopped it in between, so the pause is no longer Tether's to undo. Or the buds disconnected, which deliberately leaves it paused | Press play. Both are working as designed |
| AirPods are connected but Tether shows no battery, and the page says it is not managing them | AirPods management is off, which is the default | `tether --bt-airpods-enable on`, or the checkbox on the AirPods page |
| AirPods do not appear in the Devices list at all | They are not connected, or BlueZ has never read their SDP record so there is no Modalias and the name does not contain "airpod" | Connect them, then `tether --bt-devices` -- the row carries an `airpods` flag when Tether recognizes them |
| Calls work but the audio is on the iPhone | Working as designed. BlueZ signals the call and never opens the voice link, so there is nothing to route here | Nothing. `./scripts/bt-probe.sh --calls` during a call shows the evidence; "Getting the call audio onto the desktop instead" is the trade if you want it |
| The Calls page is empty after configuring PipeWire for call audio | PipeWire's telephony D-Bus service is off, so neither stack exports anything Tether can read | Set `bluez5.telephony-dbus-service = true` and reconnect. With it on, Tether drives PipeWire's gateway directly and the Calls page works, minus carrier and signal |
| Configured PipeWire for call audio and the machine is not in the iPhone's audio picker | `a2dp_sink` is missing from `bluez5.roles`. iOS only speaks hands-free to a machine it considers an audio destination | Add `a2dp_sink`, and accept that the phone's music comes here too -- see "Getting the call audio onto the desktop instead" |
| `hfp_connect() unable to start connection` in the bluetoothd log | PipeWire's `hfp_hf` role and BlueZ's built-in profile are both claiming UUID `0000111e` | Remove `hfp_hf` from `bluez5.roles` -- see "Calls" and 2026-09-04 |

### Keeping the phone's audio on the phone

**Recommended for every Tether user, with or without AirPods.** Stock PipeWire makes the computer
a Bluetooth speaker and hands-free unit for the phone, and that costs three things:

| | Stock WirePlumber | Recommended roles |
|---|---|---|
| iPhone calls, music and system sounds | Play on the computer | Stay on the phone |
| Tether call control | None: PipeWire takes hands-free, BlueZ exports no `Telephony1` | Works, on BlueZ |
| Carrier, signal, phone battery | None | Shown |
| AirPods handed back to this machine | iPhone falls back to the computer; pick the AirPods in Control Center each time | iPhone stays on the AirPods |
| Call audio on the computer | Offered, played only if PipeWire's call handling is set up | No: on the phone or its headset |

The stock default, from `man pipewire-props` (PipeWire 1.6.8):

```
bluez5.roles = [ a2dp_sink a2dp_source bap_sink bap_source bap_bcast_sink bap_bcast_source hfp_hf hfp_ag ]
```

Once the Classic link is up, iOS routes to a bonded device that offers itself as a speaker or
headset, the way it does to a car. Tether only brings the link up; the routing decision is the
phone's. Every desktop with Bluetooth audio behaves this way. With the phone connected,
`pactl list cards` shows it as `bluez_card.<ADDR>`.

The fix is to stop advertising the roles a phone connects to, while keeping the ones headphones
use. On WirePlumber 0.5 and later:

```bash
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cat > ~/.config/wireplumber/wireplumber.conf.d/51-no-phone-audio.conf <<'EOF'
monitor.bluez.properties = {
  bluez5.roles = [ a2dp_source bap_source hfp_ag ]
}
EOF
systemctl --user restart wireplumber
```

Then turn Bluetooth off and on on the iPhone. Only one file in `wireplumber.conf.d` should set
`bluez5.roles`; remove the key from any other. On WirePlumber 0.4 the same setting goes in
`~/.config/wireplumber/bluetooth.lua.d/51-no-phone-audio.lua` as
`bluez_monitor.properties["bluez5.roles"] = "[ a2dp_source bap_source hfp_ag ]"`.

Role names are from this machine's perspective. `a2dp_source` and `hfp_ag` drive headphones:
playback and the headset microphone. `bap_source` is LE Audio playback; drop it if nothing here
uses LE Audio. `a2dp_sink`, `bap_sink`, `asha_sink` and `hfp_hf` make the machine a destination
for a phone, and leaving them out leaves the iPhone nothing to route to. Leaving out `hfp_hf` is
also what hands BlueZ the hands-free profile, which is where call control and the indicators come
from -- see "Calls".

The one thing given up is the phone using the computer as a speaker or speakerphone. If you want
call audio on the desktop, `a2dp_sink` is mandatory for it, and with it the phone's music and
sounds come here too, and AirPods handoff falls back to the computer; see "Getting the call audio
onto the desktop instead".

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

**Corrected 2026-09-07: `Notifying` does outlive the link.** When only the LE
bearer drops under a device that is still Classic-connected, BlueZ leaves it set
indefinitely -- measured, see that entry below. `le_link_up()` now pairs it with
`ServicesResolved`.

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

The pull sends `Fields: [N, FN, TEL, EMAIL]`, the only properties `parse_vcards` reads.
Photos are the bulk of a phonebook's bytes and nothing here looks at them: on a 1441-contact
iPhone with 165 photos, an unfiltered pull was 3,604,655 bytes in ~45 s, and the filtered one
198,780 bytes in 4 s. Unfiltered, that already sat close enough to the 60 s transfer cap that
MAP polling sharing the ACL pushed it over, which is the normal state right after connect,
when the pull runs.

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
827  bluez_card.02_00_00_00_00_01  module-bluez5-device.c
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

### 2026-09-06 - Hands-free was never reconnected after the phone brought the link back

Reported as "the iPhone has not connected Hands-Free", on a machine where it had been
connected minutes earlier. Three separate things, measured on bluez 5.87,
pipewire 1.6.8, wireplumber 0.5.17, `bluetoothd --experimental` with the hfp plugin
loaded.

**Stock WirePlumber takes the profile.** Default `bluez5.roles` already contains
`hfp_hf` and `a2dp_sink`, so no configuration is needed for PipeWire to claim
`0000111e`. With no file in `wireplumber.conf.d` at all, a phone-initiated link went to
`/Profile/HFPHF` -- `spa.bluez5.native` logging `rfcomm_hfp_hf: AG indicator state:
service = 1` -- and a live call played on the desktop as
`api.bluez5.profile = "headset-audio-gateway"`, 24000 Hz mono, with the laptop
microphone linked back to the phone. BlueZ exported no telephony object, so
`Calls (HFP)` read `no` with nothing wrong on the phone. The 2026-09-04 entry's summary
that the built-in profile "wins" describes the outbound direction only; the direction an
iPhone actually uses is the other one. `--noplugin=hfp` was never applied here and was
not needed, which also retires "all three settings are required" from the PipeWire
walkthrough.

**Dropping one role is enough, and it need not cost the music.**
`bluez5.roles = [ a2dp_sink a2dp_source bap_sink bap_source asha_sink hfp_ag ]`
gave `telephony0` and `Calls (HFP): yes` with carrier, signal and battery, while the
phone's music still arrived over A2DP AAC at 44100 Hz stereo. Keeping `a2dp_sink` is
what lets the phone stay an audio destination; only `hfp_hf` has to go.

**BlueZ connects hands-free once, on a fresh BR/EDR connect.** After a Bluetooth toggle
on the phone, the phone reconnected on its own and brought everything except
hands-free:

```
bluetoothd: Device is already marked as connected
bluetoothd: .../sep3/fd0: fd(41) ready
```

Nothing retries it, and on a live ACL there is nothing to retry with:
`Device1.Connect()` returns success and does nothing, `ConnectProfile("0000111e")` fails
`No more profiles to connect to` because BlueZ matches the remote's UUID list and the
iPhone advertises `0000111f`, and `ConnectProfile("0000111f")` returns success and does
nothing. `org.bluez.Bearer.BREDR1 Disconnect` recovers it: the reconnect that follows is
a fresh profile connect and carries hands-free, while LE and its ANCS subscription stay
up throughout.

`BearerSupervisor` now does that itself, `HFP_ABSENT_SECONDS` after BR/EDR comes up with
no telephony object and only while call control is on, because the cycle costs the MAP
and PBAP sessions on that bearer. It is one cycle per outage: the drop it causes runs
back through `reset()`, so the spent flag survives `reset()` and clears only when a
telephony object is actually seen. A phone that never offers hands-free -- the PipeWire
case above -- is therefore asked exactly once and then left alone.

### 2026-09-07 - The bond re-pinned itself to BR/EDR, and re-pairing could not clear it

| | |
|---|---|
| Controllers | MediaTek, manufacturer 2279, HCI version 13 (Core 5.4); Realtek RTL8852BE, `usb:v0BDApB85C`, manufacturer 93, HCI version 11 (Core 5.2) |
| BlueZ | 5.87 on both |
| Phones | iPhone 17 / iOS 26.6.1; iPhone, iOS 26 |
| Tether | 0.2.24 and 0.2.26 |

[#128](https://github.com/zackb/tether/issues/128), two reporters, one symptom: a
`BR/EDR + LE` bond with `bond_has_le: true`, MAP and PBAP both open, the solicitation
confirmed on air (`advertising_active_instances: 1`), and `LE: no` indefinitely. Both were
shown `LE_PHONE_SILENT_ADVICE` and cycled the iPhone's Bluetooth -- "countless times", one
of them -- and both re-paired repeatedly. Neither was on a dongle. One of the two has two
other machines on which the same phone works.

**The controller is not the variable.** MediaTek and Realtek, two Core versions apart, two
distributions, identical failure. No known-bad-controller list is warranted by this, and the
2026-09-04 decision not to keep one stands.

The variable was `PreferredBearer`, and this side wrote it. `connect_classic()` pinned the
bond so the ACL would come up first:

```cpp
if (!device->le_link_up())
    set_preferred_bearer("bredr");
```

The 2026-08-25 entry fixed the pairing path -- it hands the preference back to `"le"` after
`CLASSIC_SETTLE_SECONDS` -- and named this exact remaining hole: "`connection.cpp` can re-pin
`bredr` on the Classic `Device1.Connect` fallback... it can undo the fix later in a session."
Nothing ever wrote it back. That makes the pin self-reinforcing:

1. Pair. The preference goes back to `"le"` and LE works.
2. Classic drops, for any reason at all.
3. The supervisor retries, LE is down, so the fallback re-pins `"bredr"`.
4. A bond pinned to `bredr` does not accept the inbound LE link the iPhone opens in answer to
   the solicitation, measured as an A/B on 2026-08-25 (`bredr`: down for 180s across twelve
   polls; `le`: up in 12s). LE never comes up.
5. LE being down is the condition in step 3. Every later attempt re-pins.

Which is why re-pairing did nothing for either reporter, and why the troubleshooting row that
told them to re-pair was wrong. One reporter's log is the whole cycle in forty minutes:
`11:35:42 already_paired` -> `settling` -> `soliciting`, first Classic failure at `11:36:52`
(`br-connection-aborted-by-local`), and LE never up again, across a daemon restart.

Fixed in `BearerSupervisor::tick()` rather than in the fallback, because the fallback's reason
for pinning is sound and only the hand-back was missing. It reuses the settle gate that already
guards the outbound LE dial, and takes both guards the earlier entries paid for: never over a
dial in flight (2026-08-19: that write is what produced `le-connection-abort-by-local`, an error
that appears in this reporter's log too), and once per Classic session rather than once per poll.
Older bonds are cleared without a re-pair, which the previous troubleshooting row said was
impossible.

**Two failures of reporting made this cost far more than it should have.**

`PreferredBearer` appeared nowhere -- not in `--bt-connection`, not in `--bt-diagnostics`, not in
`bt-probe.sh`. Two reporters produced about 25KB of logs, a full diagnostics dump and a clean
probe between them, and the one property that decided the outcome was in none of it. It is now
parsed onto `Device`, emitted as `preferred_bearer`, and checked per bonded device by the probe,
which names the `busctl` one-liner when it reads `bredr` with LE down.

`LE_PHONE_SILENT_ADVICE` sent both of them at the phone. Its selection is sound as far as it goes
-- `solicited_since_le_down_` correctly rules out our own advertisement -- but "not us" was then
read as "the phone", and there was a third possibility sitting in a property we were writing
ourselves. A pinned bond now gets its own advice, ahead of that one. This is the second time
(2026-09-04 was the first) that a reason string confidently blamed something the daemon had not
actually checked.

One incidental, from the same log and not the cause of any of the above:

```
[11:47:50] RegisterAdvertisement failed: Le délai d'attente est dépassé
[11:48:11] RegisterAdvertisement failed: GDBus.Error:org.bluez.Error.AlreadyExists
   ... every 30s until 11:50:12
```

`register_with_bluez` leaves `registered_with_bluez` false on any error, and
`unregister_with_bluez` is gated on that flag, so a call that timed out left BlueZ holding a
registration nothing would ever release. It cleared only when the advert's own 180s `Timeout`
expired -- three minutes with nothing on air. A timed-out registration is now released (harmless
if it never took), and an `AlreadyExists` is adopted as the registration it is rather than
discarded. Not raised: the 10s D-Bus timeout on that call. A controller that stalls past ten
seconds is the problem; a longer timeout only stalls the poll loop with it.

### 2026-09-07 - A notify flag outlived its link, and the daemon stopped trying

| | |
|---|---|
| Controller | MediaTek `usb:v0E8Dp0717`, HCI version 13 |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone 15 Pro, iOS 26 |

Set up to test the `PreferredBearer` hand-back from the entry above: pin a healthy
bond to `bredr`, drop the LE bearer, watch the supervisor put it right. It did
nothing for 90 seconds. The hand-back never logged, no solicitation went on air,
and `tether --bt-connection` reported `LE: yes` and `Notifications: yes` the
whole time -- on a link that was measurably down.

The two views disagreed completely:

| | Daemon | Bus |
|---|---|---|
| LE | `le_connected: true` | `Bearer.LE1.Connected: false` |
| ANCS | `ancs_ready: true` | `ServicesResolved: false` |

`Device::le_link_up()` is `le_connected || ancs_notifying`, and `ancs_notifying`
is read from the ANCS Notification Source characteristic's `Notifying` property
with nothing else consulted. The 2026-08-19 entry chose that property on the
reasoning that "a notify session needs a live ATT link to exist, so it cannot
outlive the link the way a cached attribute does." **That is not true when only
the LE bearer drops.** `Bearer.LE1.Disconnect` under a device still connected on
BR/EDR leaves everything standing:

| Signal | LE up | LE down |
|---|---|---|
| `Device1.Connected` | true | true |
| `Bearer.LE1.Connected` | true | false |
| `Bearer.BREDR1.Connected` | true | true |
| GATT characteristics under the device | 23 | 23 |
| ANCS notify sources with `Notifying` | 2 | **2** |
| ANCS UUID in `Device1.UUIDs` | present | present |
| **`Device1.ServicesResolved`** | **true** | **false** |

Watched to 38s and again to 90s in a separate run; nothing cleared. So
`le_link_up()` latched true on a dead link, and everything that recovers LE lives
behind `!le_connected` in `BearerSupervisor::tick()` -- the outbound dial, the
ANCS solicitation, and the bearer hand-back. All three were unreachable.
`ancs_soliciting` read false throughout: nothing was on air, and nothing was
asking.

This is the shape of the long-standing "LE drops and two times in three I have to
toggle Bluetooth on the iPhone" complaint on this machine. The daemon was not
failing to recover the link; it did not believe anything was wrong. A phone-side
toggle is the only thing that tears the GATT tree down and clears `Notifying`,
which is exactly why it was the only remedy that ever worked, and why waiting
never did.

`ServicesResolved` is the one property that tracks the ATT link, so it is now
required alongside the notify flag:

```cpp
bool le_link_up() const { return le_connected || (ancs_notifying && services_resolved); }
```

The `ancs_notifying` half stays, because `Bearer.LE1.Connected` still reads false
on a link the phone opened inbound (2026-08-19), and the cached-GATT trap that
sent the original reading there is unchanged -- a cached tree has no notify
session. `ServicesResolved` alone would not do either: 2026-08-23 caught it false
on a live `Bearer.LE1` link, but with no GATT objects at all, so the notify half
is false there and the pair still reads down correctly.

Re-running the same test with both fixes installed:

```
18:02:38  pinned to bredr, Bearer.LE1.Disconnect
18:02:41  bond was pinned to BR/EDR; preference handed back to LE
18:02:43  soliciting ANCS for 180s
          LE, ServicesResolved and PreferredBearer all back inside 5s
```

Against 90 seconds of nothing on the same hardware minutes earlier. That is the
first end-to-end confirmation of the hand-back as well: it could not fire before,
because the branch it lives in was unreachable.

Not settled: `ServicesResolved` on a live *inbound* LE link, where
`Bearer.LE1.Connected` reads false. The unit fixture asserts true on the mechanism
argument that a delivering notify session means discovery completed on that link.
It has not been captured. Anyone who catches that state should read the property
and record it here.

### 2026-09-07 - The replay dedupe outlived the UIDs it was deduping

| | |
|---|---|
| Controller | MediaTek `usb:v0E8Dp0717`, HCI version 13 |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone 15 Pro, iOS 26 |

With the LE link finally staying up, notifications stopped arriving, and
dismissing one from the UI answered:

```
ancs: control point write failed: GDBus.Error:org.bluez.Error.Failed: Operation failed with ATT error: 0xa2
```

`0xa2` is ANCS **Invalid Parameter** in Apple's control point error range
(`0xa0` Unknown Command, `0xa1` Invalid Command, `0xa2` Invalid Parameter,
`0xa3` Action Failed): the NotificationUID does not name anything on the phone.
The daemon log shows the boundary exactly -- `Not connected` while LE was down at
21:37, then every write answering `0xa2` from 21:55 once it was back.

**ANCS UIDs are scoped to the connection.** The retained list made that plain:

```
uid=9   Pokémon GO   Raid Invitation ...
uid=29  Pokémon GO   Raid Invitation ...   <- the same notification, next session
uid=3   Wallet       Apple Card
uid=12  Google       ...
```

Eight notifications carrying uids 3, 9, 12, 13, 16, 17, 18 and 29 -- small
counters that restart low each session, with one notification present twice under
two of them.

`NotificationRegistry::seen_` is commented "UIDs seen this session", and nothing
made that true. `clear()` is the only thing that empties it, and `set_device()`
calls it only when the bond is a *different phone*. Deliberately so: the comment
there explains that wiping the registry on a blip would empty the notification
list and the phone's replay would then be suppressed as pre-existing, so nothing
would come back. That reasoning is right about the notifications and wrong about
the dedupe, and the two live in the same object.

So `seen_` accumulated every UID from every session, and `classify()` --
`seen_.count(event.uid) ? Ignore : Fetch` -- read a genuinely new notification
landing on a recycled counter as a replay and dropped it before any fetch. Each
reconnect saturates more of the low range. That is a phone that quietly stops
mirroring, with a healthy `ancs_ready: true` above it.

The same session boundary explains the dismissal: the list holds UIDs the phone
retired, and `PerformNotificationAction` on one of them can only answer `0xa2`.

Split the two lifetimes:

- `begin_session()` clears `seen_` and bumps a session counter, called for every
  new LE session. What is on screen is untouched, so the blip behaviour the
  earlier comment protects is unchanged.
- `Notification::session` records which session issued a stored UID.
  `perform_action()` on a stale one retires the desktop copy and never writes to
  the control point -- the phone's copy is unreachable by that UID, and retiring
  the local one is what dismissing it asked for.

The full `clear()` still runs, still only for a genuinely different phone.

Worth separating from the diagnosis: this bug is older than the LE work above and
was largely invisible behind it. A link that came back only after someone toggled
Bluetooth on the phone rotated sessions rarely; one that reconnects on its own
rotates them constantly, and the stale dedupe fills up in hours. Fixing the link
is what surfaced it.

### 2026-09-08 - The session reset was bound to the one event that does not fire

| | |
|---|---|
| Controller | MediaTek `usb:v0E8Dp0717`, HCI version 13 |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone 15 Pro, iOS 26 |

The entry above got the diagnosis right and the trigger wrong. Notifications
still stopped arriving after a reconnect, and dismissing one still answered:

```
ancs: control point write failed: GDBus.Error:org.bluez.Error.Failed: Operation failed with ATT error: 0xa2
```

`begin_session()` had exactly one call site, `AncsClient::set_device()`, and that
function opens with:

```cpp
if (state_->device_path == device_path)
    return;
```

`sync_ancs()` is built to keep that path steady. The comment there says so
plainly -- the LE bearer flaps faster than a GATT subscription can be rebuilt, so
the path is held for `ANCS_BEARER_GRACE_SECONDS` (30) rather than dropped on
every blip:

```cpp
const bool hold = ready || le_up || now - le_down_since < ANCS_BEARER_GRACE_SECONDS;
```

So the reconnect that rotates the phone's UIDs hands `set_device()` the same
string it already holds, and the function returns before reaching the reset. The
two safeguards were installed on the one path that a healthy link does not take.

Two more session boundaries never went near `set_device()` at all:

| Path | What it does | Went through `set_device()` |
|---|---|---|
| `verify_subscription()`, `BlueZ cleared Notifying` | `subscribed = false`, `tick()` resubscribes | no |
| `verify_subscription()`, characteristics gone | `drop_gatt_paths()`, rediscover, resubscribe | no |
| `subscribe()` answering `UnknownObject` | `drop_gatt_paths()` | no |
| LE down past the grace window | path empties, then returns | yes |

Only the last row -- a link down for more than 30 seconds -- ever reset anything.
`initial_sync` sat on the same trigger and had the same hole, so a silent
resubscribe also stopped recognising the phone's replayed backlog as a backlog.

The boundary is the subscription, not the path. ANCS numbers notifications per
GATT connection, so the UID space is new exactly when `StartNotify` succeeds
again. `begin_session()` and `initial_sync = true` now run there, in `tick()`,
which is the single place `subscribed` goes from false to true, and every row of
the table above passes through it. The `set_device()` call is kept for the case
where BlueZ leaves `Notifying` set on a dead link and the path is what drops
first; a redundant bump costs a cleared dedupe, which the `initial_sync` reset
makes harmless.

The log now marks the boundary that had none:

```
ancs: notification session 3 started
```

Worth separating from the fix: nothing about the earlier diagnosis was wrong, and
its tests pass both before and after this change. They exercise
`NotificationRegistry`, which was already correct. What was untested, and
untestable without a phone, is *when* the registry is told a session began -- and
that was the whole defect. A test that calls `begin_session()` directly can never
catch a `begin_session()` that is never called.

### 2026-09-08 - Out-of-range locking, and why RSSI is not how you find range

Issue #153 asked for the session to lock when the phone leaves, with an RSSI
threshold as the trigger. RSSI is not available to this daemon, measured against
BlueZ 5.87 with the phone bonded and connected:

```
$ busctl --system get-property org.bluez /org/bluez/hci0/dev_02_00_00_00_00_01 \
    org.bluez.Device1 RSSI
Failed to get property RSSI on interface org.bluez.Device1: No such property 'RSSI'
```

`Device1.RSSI` is *declared* in the interface's introspection, which makes it look
available; it is only ever set from discovery reports, and never for a device that
is connected. It is absent from `GetManagedObjects` for all three devices on this
machine, including the connected iPhone, and stays absent through eight seconds of
active discovery. The kernel does hold the number, and it is out of reach:

```
$ btmgmt --index 0 conn-info 02:00:00:00:00:01
Get Conn Info failed, status 0x14 (Permission Denied)
```

mgmt `Get Conn Info` wants `CAP_NET_ADMIN`. Reading connected RSSI at all means a
privileged helper, for a number that would still only be a proxy for the question.

BlueZ answers the question outright instead. `Disconnected(name, message)` carries
the kernel's mgmt disconnect reason, on `org.bluez.Device1` and, with the
experimental bearer API up, on `org.bluez.Bearer.LE1` and `Bearer.BREDR1` -- all
three on the same device object path:

| Reason | Kernel event | What it means here |
|---|---|---|
| `org.bluez.Reason.Timeout` | `MGMT_DEV_DISCONN_TIMEOUT` | link supervision timeout: the phone left |
| `org.bluez.Reason.Local` | `MGMT_DEV_DISCONN_LOCAL_HOST` | we hung up: `disconnect_classic`, adapter powered off, rfkill |
| `org.bluez.Reason.Remote` | `MGMT_DEV_DISCONN_REMOTE` | the phone hung up: Bluetooth off, airplane mode, battery |
| `org.bluez.Reason.Suspend` | `MGMT_DEV_DISCONN_LOCAL_HOST_SUSPEND` | this machine is going to sleep |
| `org.bluez.Reason.Authentication`, `Unknown` | | auth failure, controller catch-all |

Only `Timeout` locks. That the other reasons exist is what makes the feature small:
`Local` already covers the HFP bearer cycle this daemon performs on itself
(`HFP_ABSENT_SECONDS`, once per daemon life) and the radio being switched off, and
`Suspend` already covers sleep -- so there is no "we initiated this" latch to keep
and no `PrepareForSleep` hook to add. Both were in the first design and both came
back out.

`Device1.Disconnected` is a plain `GDBUS_SIGNAL`, not experimental-gated, so it is
there in compatibility mode too. One subscription with a null interface filter and
member `Disconnected` catches all three interfaces, and the handler reads which one
fired from its `interface_name` argument. This is the first thing in `monitor.cpp`
that reads a signal's payload rather than treating it as a "something moved" ping.

Locking waits for every path to be down -- BR/EDR, LE, and any open OBEX session --
and then for `lock_away_seconds` (30 by default). A bearer that times out while the
other carries on is a flap, which is the same thing `ANCS_BEARER_GRACE_SECONDS`
exists for. Coming back inside the window clears the arm and forgets the reason, so
a returning phone is never judged on why its previous link ended.

The lock itself is `org.freedesktop.login1.Session.Lock` on
`/org/freedesktop/login1/session/auto`, on the *system* bus, which resolves to the
daemon's own session with no session id to look up. `LockedHint` is read first so an
already-locked session is left alone. logind only emits a signal, so a compositor
with no lock handler configured -- hypridle, swayidle -- will do nothing with it;
`lock_command` in `bluetooth.json` runs a command instead for those setups. No
Wayland protocol is involved: implementing `ext-session-lock-v1` would mean shipping
a screen locker, which is not this daemon's job.

### 2026-09-10 - `--bt-setup` printed systemd commands on machines without it

Requested as #158 for Artix. Both setup steps assumed systemd: the bearer API step wrote a
`bluetooth.service.d` drop-in, and the class step enabled `tether-btclass@`. On Artix the Arch
package still installs the unit into `/usr/lib/systemd/system`, so `set_class_command()`'s
probe found it and printed `systemctl enable` on a machine with no `systemctl`.

BlueZ reads both settings from `main.conf` `[General]` under any init. `Experimental = true`
sets the same option `--experimental` does. `Class` sets the adapter's default major and
minor class, but the `hostname` plugin rewrites the class from the chassis type
`org.freedesktop.hostname1` reports, which is why it cannot stand in for the unit where
hostnamed runs; the Computer / Laptop `0x7c010c` recorded on 2026-07-16 matches that. With no
hostnamed the plugin has nothing to apply, so `main.conf` should hold across a restart.

`systemd_booted()` picks the branch by testing `/run/systemd/system`, as `sd_booted()` does,
rather than looking for `systemctl` on PATH. Without systemd the class step is a `sed` on
`main.conf` plus `echo | sudo btmgmt --index hci0 class 4 8` for the running adapter, and
the bearer step is a `sed` plus a restart through the machine's own service manager.

Not yet verified on hardware: that `class=ok` survives a `bluetoothd` restart on a machine
without hostnamed. The `sed` leaves a `main.conf` with no `Class` or `Experimental` line,
commented or not, unchanged, and `--bt-setup` keeps listing the step. The experimental half of
that last point is fixed in 2026-09-20 below: the flag is read off BlueZ now, whichever way it
was set.

### 2026-09-11 - The AirPods sink jammed after a call

Reported in #85 and reproduced on AirPods Pro 3: music on Linux, a call on the iPhone,
playback paused, and after the call playback resumed into silence. PipeWire stayed jammed
until the buds were reconnected.

The reporter captured the working sequence from their own implementation at three stages:

| Stage | Card profile | `MediaTransport1` | Sink |
|---|---|---|---|
| Before the call | `a2dp-sink-sbc_xq` | `active` | index 468, RUNNING |
| During | `off` | `idle` | gone |
| After | `a2dp-sink-sbc_xq` | `active` | index 623, RUNNING |

The transport object never goes away; the sink does, and comes back as a new object. A sink
left in place while the phone holds the transport is what jams. A first fix detected a
suspended sink with a stream on it after the call and cycled the profile off and on two
seconds apart. It worked, but it guessed at the jam, ran on a separate thread from the
ownership reclaim with both resuming players, and changed the profile twice in quick
succession, which the Apple-mode firmware tolerates badly. The card now goes off when the buds
are given up and back on when they are taken back.

That fix could only run from call state, because the ownership path never fired on these
buds: `active()` tested `state >= 0x17` and Pro 3 reports `0x07`. That also left
`taking_over()` false while the iPhone held the buds, so nothing in the peer state blocked a
claim. Reading bit `0x02` fits every value recorded so far and puts iPhone media on the same
path as calls.

Volume, measured: WirePlumber restores a Bluetooth route's saved volume when the new sink's
route comes up (`scripts/device/state-routes.lua`, "a new route is now active, restore the
volume"), and these buds had `channelVolumes 0.174729` saved in
`~/.local/state/wireplumber/default-routes`. The reporter saw a rebuilt sink come up at a
default volume on their stack, so the raw value is saved on release and restored before
resuming anyway, then read back.

The first hardware run of that, 09:51 the same day, jammed again. The log shows why:

- 09:51:21 the iPhone joined the host list at `0x00` and 200ms later BlueZ reported the buds
  disconnected from this machine, `org.bluez.Reason.Remote`. No handoff, a link drop.
- 09:51:43 the buds were back, with the iPhone at `0x02`. The owner bit alone released them, and
  nothing was switched off: the card had not come back to an A2DP profile yet.
- 09:51:56 WirePlumber logged `Failure in Bluetooth audio transport` on the new node, and the
  buds reported the iPhone's call.
- After the call ended no connected-devices notification arrived at all. The iPhone stayed at
  `0x02`, so a reclaim waiting on the owner bit never fired. Even had it fired, its claim
  would have been dropped, because counting the owner bit as `taking_over()` blocked claims.

So handoff now yields to a peer that owns the buds **and** has audio on them, from the
audio-source notification with the same three-second settle the call tracking already had;
`taking_over()` is back to `0x10` and up; and a reclaim with nothing switched off on release
switches the card off before switching it on.

The second run, 10:05 to 10:10, was worse:

- 10:08:49 and 10:10:09 the reclaim claimed the buds back six seconds after a single audio
  source none, while the iPhone still read `0x07` and its call was still going.
- The night before, 20:33:39, the iPhone took `0x07` for a call and still held it at 20:37:48,
  with Linux playing in between. The owner bit does not mark the end of a call either.
- Calls at 09:51:10, 10:05:31 and 10:08:56 put SCO on this machine (`corrupted SCO packet` in the
  kernel log). That was first read as "iOS routed the call here"; it is not. Measured at 17:09 and
  17:10, the same lines appear while the call plays in the buds, so they only say the iPhone set
  up a hands-free link with the laptop. At 10:05 the iPhone read `0x01` while an iMac on the same
  iCloud account held the buds at `0x07` playing media, which is the better explanation of why
  the call went to the phone's speaker: the buds' second slot was taken.
- 10:05:15 the claim a session sends when it opens took the buds from that iMac mid-media,
  while this machine had given them up.

So the call list decides when call control is on, and a watcher that has given the buds up
sends no claim of its own. Several Apple devices on one account is the normal case.

The third run, 11:11 to 11:14, handed over and back correctly on every call, but music played
briefly from the laptop speakers at the start of each. At 11:13:12 WirePlumber logged the A2DP
transport failure at 12.000 and the release, which pauses first, logged at 12.053; at 11:13:35,
35.569 and 35.626. SCO had reached this machine 300 to 550ms before each failure, so the phone
was already committed to the call while Tether waited for its one-second call-list tick.

Later the same day the reporter sent the source of their fork, AC v2.3.0, whose
`DOCUMENTATION.md` records ~15 versions spent on these same failures. Three things it does that
Tether did not, now adopted: the host registration packets above, re-sending configuration after
each handoff, and a three-second claim cooldown rather than one.

Its B-field table briefly retired the owner-bit reading here, which was a mistake, corrected the
same evening by five handoffs at 17:09 and 17:10: our own entry alternates `0x00` and `0x02`, the
iPhone `0x05` and `0x07`, each transition arriving with the matching verdict packet. Bit `0x02` is
ownership on this hardware whatever it means on theirs, and the wire protocol section above now
records the measurement rather than either guess.

The suspicion that Tether's own LE work spoils call routing, taken from the fork's v2.3.0 entry,
does not survive measurement either. Three calls dialled on the iPhone with the buds attached to
this machine -- tetherd stopped, tetherd running, tetherd running with ANCS off -- all routed to
the AirPods. An earlier round of the same test looked damning until the logs showed the buds had
dropped off this machine before two of the three runs; a run where the buds are not attached here
proves nothing, so check `Device1.Connected` before each call.

Two of its findings are not adopted. It releases A2DP proactively during a three-second grace at
connect, with a 15s watchdog, to dodge a firmware disconnect timer this project has not measured.
And its own v2.3.0 entry reports that its BLE scan, run while Tether brings up its BR/EDR link,
leaves that link degraded and iOS then refuses the buds for calls -- its fix was a startup delay.
Whether Tether's own LE work does the same to it is worth measuring before any code changes.

Not yet verified on hardware: the full sequence on Pro 3, iPhone media rather than a call,
the owner bit on Pro 2, and whether an `off` profile persists if tetherd dies while the buds
are away. WirePlumber restores a saved `off` profile unless `session.dont-restore-off-profile`
is set.

### 2026-09-11 - The Class of Device unit was pinned to an index the adapter had left

`tether-btclass@hci0` started failing with `Invalid Index` on a machine where it had worked
for months. Nothing in Tether had changed. The kernel log has the whole story:

```
Bluetooth: hci0: Execution of wmt command timed out
Bluetooth: hci0: Failed to send wmt func ctrl (-110)
usb 3-5: reset high-speed USB device number 4 using xhci_hcd
Bluetooth: hci1: Device setup in 1693823 usecs
```

The MediaTek controller's firmware handshake timed out, the USB device was reset, and the
controller came back under the next free index. `hci0` was gone; `/sys/class/bluetooth` held
only `hci1`. A unit instantiated on the index cannot survive that, and the failure reads like
a missing adapter rather than a renamed one.

The instance name is now a hint rather than the target. Each attempt uses `%i` if
`/sys/class/bluetooth/%i` exists and otherwise takes the first adapter present, so an already
enabled `tether-btclass@hci0` keeps working under any index and every documented command
stays as written. Resolving inside the loop rather than once also covers a re-enumeration
that lands mid-loop. The retry window went from 10s to 30s: the reset above put the new
adapter on the bus 13s after boot, which the old window missed even with a correct name.

`$$` in `ExecStart` is a literal `$` -- systemd expands `$a` as one of its own variables and
would hand the shell an empty string.

Not covered: a controller that re-enumerates at runtime with no `bluetoothd` restart. Nothing
re-runs the unit there, and `bluetoothd` applies its own default class to the new adapter, so
the class is wrong until `bluetooth.service` restarts. A udev rule tagging adapter `add` with
`SYSTEMD_WANTS` would close that, and would also remove the manual enable step.

### 2026-09-12 - The buds were taken at boot and never given back

Reported on 0.2.30 against both Pro 2 and Pro 3 (issue #85): after starting the machine, the
iPhone could not see the AirPods at all, and the only way back was forcing the connection
from the phone's own Bluetooth menu.

Every session claims ownership five seconds after the channel opens
(`CLAIM_SETTLE_MS`). The claim is gated on `peer_taking_over` and `yielded` and on nothing
else -- not on whether this machine has any audio, and not on `airpods_handoff`. An idle
iPhone sits at `0x15`, which `taking_over()` excludes on purpose, so nothing blocked it. On
the other side, `ownership_action()` only gives the buds up for a peer that owns them *and*
has audio on them, so a phone that was merely idle never got them back. The machine held
ownership from boot until the user intervened.

Not a new failure: the same claim is what took the buds from an iMac mid-media on 2026-09-11
below. The fix then was the `yielded` flag, which is false on a fresh daemon and on a first
connect -- exactly the boot case.

The claim itself has to stay, for the Pro 3 validation reason above. What was missing is the
other half of the arbitration: three seconds after the claim, ownership goes back unless the
buds have reported an audio source for this machine. `releases_when_idle()` in
`src/core/src/bluetooth/airpods.cpp`, and `PeerSummary::present` so a session alone with the
buds keeps them.

Deliberately not touching the card profile on this path, unlike the call handoff. Releasing
it destroys the sink, and with no bluez sink the next thing the user plays goes to the
speakers, the buds never report an audio source for this machine, and the claim for local
playback can never fire. An idle `MediaTransport1` does not stop the phone taking the buds.

**Verified on hardware the same day, Pro 3, twice**: with the phone owning the buds and both
sides quiet, `claim sent` at opened+5s, verdict `0x01`, `nothing playing here, handing ownership
back` at +3.003s, verdict `0x00`, phone back to `0x07`. A pod out and back in without closing the
case left the link up past thirty seconds, so releasing straight after the validation claim does
**not** cost this host its validation. No gating to `0x10` and above is needed.

Three preconditions silently decline the release, and each one blocked a test run before the
first success. A peer must be in the buds' host list, or there is nobody to hand them to. Nothing
may be streaming here -- a *paused* Electron player still holds an uncorked stream and the buds
still report media for this machine, so such a player has to be quit, not paused. And the phone
must be quiet, or `peer_busy()` releases at ~0.4s, long before the validation claim is due.

### 2026-09-12 - Four more, from testing the release above

**The release tore the sink down over an idle card.** `release_bluez_card()` ran on every
handoff. When this machine was not playing, that left no sink, so the next thing the user played
went to the speakers, the buds never reported an audio source for this machine, and the claim for
local playback could never fire -- once measured at 3m52s of dead audio. The teardown now needs
`AirPodsState::local_audio`, the buds' own view of whether they carry this machine's audio.
`paused > 0` was tried first and is wrong: a browser tab streams with no MPRIS player at all, and
leaving that running pumps audio into buds just given away, which produced a release/reclaim
ping-pong every time the phone paused.

**The card teardown raced the pause.** MPRIS `Pause` is a request; the player keeps feeding the
sink for a moment after it returns, and a card switched off under a live stream has the audio
server move it to the speakers. `audio::sink_quiet()` waits up to `SINK_QUIET_TIMEOUT_MS` (300)
for the sink to leave `RUNNING` first. That removed the burst on the call path. The bound is
short because the buds still have to reach the phone before iOS picks the call's route; it is a
bounded wait, not a confirmation, and `still running after 300ms` does appear for Electron
players without being audible.

**Phone media gives no warning, and cannot be fixed here.** `AirPodsState::owns` now carries this
machine's owner bit from the host list, and losing it pauses MPRIS at once -- 471ms earlier than
waiting for the release decision. It is not enough. The phone takes the A2DP transport at the
same instant it takes ownership, so the host-list packet is already past tense and the audio
server rescues the stream before a D-Bus pause reaches the player. A call is fixable only because
the call list fires while it is still ringing. The remaining lever is audio-server side: stopping
PipeWire rescuing streams off a sink that vanishes, PulseAudio's `module-rescue-streams`
equivalent in WirePlumber's linking policy. Not attempted.

**An `off` profile outlives the daemon.** Listed as unverified under 2026-09-11; it is real and
survives a reboot. WirePlumber saves the profile a handoff switched off and restores it on the
next connect, so the buds arrive with no sink and everything plays out of the laptop speakers. A
fresh daemon has an empty `handoff->released` and restores nothing. `audio::revive_bluez_card()`
runs on the AirPods connect edge -- not on every BlueZ property change, the check costs a `pactl`
of its own -- and switches A2DP back on for buds this run did not release.

### 2026-09-12 - A BR/EDR reconnect re-latched the LE link

| | |
|---|---|
| Controller | MediaTek `usb:v0E8Dp0717`, HCI version 13 |
| BlueZ | 5.87 with `--experimental` |
| Phone | iPhone 15 Pro, iOS 26 |

Back from out of range, MAP, PBAP and HFP all reconnected and notifications never returned.
No `notification session` line, no `0xa2`, no resubscribe: the daemon did not think anything
was wrong. `--bt-connection` read "LE: yes" and "Notification mirroring is active." The bus:

| Signal | Value |
|---|---|
| `Bearer.LE1.Connected` | false |
| `Bearer.BREDR1.Connected` | true, reconnected |
| ANCS notify sources `Notifying` | true |
| `Device1.ServicesResolved` | **true** |

The 2026-09-06 fix required `ServicesResolved` beside the notify flag because it went false
when only LE dropped. That test kept BR/EDR up. Here BR/EDR dropped too, and its rediscovery on
return set the device-wide `ServicesResolved` true again, over a cached GATT tree still marked
`Notifying`. `le_link_up()` latched through the other bearer, so the supervisor's LE recovery
was unreachable again, and `verify_subscription()`, which reads only `Notifying`, kept `ready`
true and the path held.

No property distinguishes the case. A read does:

```
$ busctl call org.bluez .../service0001/char0002 org.bluez.GattCharacteristic1 ReadValue a{sv} 0
Call failed: Not connected        (3ms)
```

`gatt_link_alive()` reads the GAP Device Name (0x2A00) and reports down only on
`Not connected`. It confirms an inferred LE link in the bearer ops' `le_connected()`, and in the
ANCS client before `StartNotify` and in `verify_subscription()`, which now drops `ready` with
`LE link gone` so the path clears after `ANCS_BEARER_GRACE_SECONDS`.

Verified by walking out of range with the fix installed:

```
15:52:49  Bearer.LE1 disconnected (Timeout)
15:53:26  Bearer.BREDR1 disconnected (Timeout)
15:53:51  ancs: the notification subscription is no longer live (LE link gone), rebuilding it
15:54:15  ancs: The iPhone is not connected over LE.
          soliciting ANCS every 180s while away
16:01:19  ancs: notification session 5 started
```

A new notification popped up afterwards with nothing touched on the phone.

Not settled: the read on a live *inbound* link where `Bearer.LE1.Connected` reads false
(2026-08-19). Any error other than `Not connected` counts as up; if BlueZ answers
`Not connected` there, this drops a working session every 30 seconds.

### 2026-09-12 - The stem swipe stayed on the phone at 0x17

Reported in #85 on AirPods Pro 2 and Pro 3, two machines: the stem swipe always changed the
iPhone's volume while music played here. Never reproduced on our Pro 3.

The reporter isolated it on their side:

- The swipe follows the host whose CLAIM the firmware last accepted. A script sending only
  handshake, set-features and `04 00 04 00 09 00 06 01 00 00 00` moved it to Linux at once.
- Every ownership change moves it again. After a call the swipe stays on the phone until the
  host claims once more.
- The host list when that claim was accepted had the iPhone at **`0x17`**:

```
040004002e00 01 00 02 7413ea6efeeb 01 01 64484254e53c 02 17
```

`taking_over()` read `0x17` as a peer taking the buds, because only `0x15` was carved out of
the `0x10` range. So on those buds `peer_taking_over` held for as long as the phone owned them,
and the session claim, the reclaim after a call and the claim for local playback were all
dropped by the gate. Our iPhone owns at `0x07`, below the range, which is why it never showed
here. The code also contradicted the rule above, that owning never blocks a claim.

`taking_over()` now excludes the owner bit. Suggested instead: send the session claim on the
features acknowledgement, bypassing the gate and the cooldown. Not taken, since the fixed gate
lets the existing claims through, and a claim before `CLAIM_SETTLE_MS` is the one that drops the
audio link.

Both gates now log when they hold a claim back.

Not settled: whether a claim into `0x17` survives a pod movement afterwards (the reference's
contested-host drop). Unconfirmed on `0x17` hardware.

### 2026-09-13 - A stem pause handed the buds to the iPhone, and nothing brought them back

Pro 3, Linux playing and owning (`us 0x03`). A stem press stopped Linux and started the iPhone.
From then on the stem drove the iPhone, and Linux had no sink until the card profile was set
by hand.

```
01:32:41.436  us 0x03 -> 0x01, peer 0x05 -> 0x07
01:32:41.439  paused 2 player(s), the phone took the buds
01:32:41.896  audio source media on the iPhone
01:32:41.976  gave the buds to the iPhone (paused 0, card a2dp-sink -> off)
01:32:43.592  audio source media on the iPhone   <- last one; no none follows
```

The players still reported `Playing` 3 ms after the ownership change, so the buds moved it
before any pause reached this machine. Tether did not cause the takeover.

Suspected first: the `tipi_media_info()` packet each session sends, which says this host has
`playingApp NA` and `hostStreamingState NO` even while it streams. **Ruled out the same night**:
with the packet removed, after a restart and again after a case cycle, the press still moved the
buds to the iPhone. The packet is back. What moves them is on the firmware or iOS side, and
not explained.

The stuck half was ours. The phone kept playing, so `peer_audio` stayed latched and no
reclaim came; the card was off, so playing here went to the speakers, the buds reported no
local source and nothing claimed.

An MPRIS poll that reclaimed on a local play edge got the buds back, and a stem pause on the iPhone
returned them about 6s later (`PEER_AUDIO_SETTLE_MS` plus `HANDOFF_GUARD_SECONDS`). It worked, and
it was more timing stacked on inference. Both were replaced the same day, below.

### 2026-09-13 - The stem config, and what the iPhone was saying all along

AirPods Center v2.3.0, orychalk's fork, sends stem config `0x39 = 01` and handles the press itself.
Without it the firmware's single press is Apple's host toggle. With it, from 10:44:25, ten presses
toggled Linux and the buds never moved.

Logging every packet no parser handled then showed opcode `0x11` arriving from the iPhone all
along, in OPACK -- see "Smart routing". The capture, from 10:43 (`tetherd.log`):

```
10:45:37.445  iPhone  com.apple.Music YES 301
10:45:42.154  us 0x03 -> 0x01, peer 0x05 -> 0x07          routed from Control Center
10:46:01.780  playback started here, taking the buds back  card was off: speaker burst
10:46:02.523  iPhone  Unknown NO 100                       the iPhone yields to the claim
10:48:01.079  iPhone  com.apple.TelephonyUtilities YES 501 ring
10:48:01.339  us 0x03 -> 0x01, peer 0x05 -> 0x07
10:49:10.948  iPhone  Unknown NO 100                       hangup
10:49:16.907  claim sent                                   6s of latch and guard
10:43:44.138  iPhone  audioRoutingSetOwnershipToFalse, reason ManualRoute
```

The phone says when a call starts and ends, and when media starts and stops, exactly and ahead of
the ownership change. Everything Tether inferred from the owner bit, the audio-source notification,
`PEER_AUDIO_SETTLE_MS`, `HANDOFF_GUARD_SECONDS`, the call list and the one-second MPRIS poll was
standing in for it. All of it is gone; "Ownership handoff" describes what replaced it.

The iPhone did not take the buds for music on its own while Linux owned them (10:45:37 to 42): it
waited for Control Center. That matches a Mac.

Also from orychalk (#85): a B-field threshold does not port (Pro 1 keeps an active iPhone at
`0x01`), which the reports make moot; forcing a claim into an owning `0x17` iPhone survived twenty
pod movements on Pro 3; and both buds out moves the volume target, hence the ear-return claim.

Not yet verified on hardware: the whole of the above.

### 2026-09-15 - An LE-only drop left notifications dead until a restart (#190)

Reported in #190, and four times in our own `tetherd.log` on 0.2.31, which carries the 2026-09-12
fix above:

```
09-13 15:02:24  ancs: Notification mirroring is active.
09-13 15:10:16  Bearer.LE1 disconnected (Timeout)        BR/EDR stays up
09-13 15:10:17  soliciting ANCS for 180s                 the supervisor saw LE go down
                no ancs line for 2.5 hours
```

Same at 09-14 10:24, 19:52 and 21:55. Each ended only with a daemon restart, whose fresh client
started a session within seconds, so LE was back and only the old subscription was dead. The
cases that recovered (09-12 walk, 09-14 18:43 suspend) dropped both bearers.

- The supervisor saw LE down: `ServicesResolved` goes false on an LE-only drop, so `le_link_up()`
  short-circuits before the read.
- `verify_subscription()` did not: it reads only the stale `Notifying` and the GAP read, and with
  BR/EDR up that read does not answer `Not connected`. `ready` stayed latched, `hold` kept the
  path, and nothing was ever rebuilt.
- Tether never sent `StopNotify`. BlueZ keeps its notify client for our sender across the drop,
  so a resubscribe would be a no-op on that registration; a restarted daemon is a new sender and
  gets a real CCCD write.

The ANCS client now drops the session on the `Disconnected` signal for `Bearer.LE1` or `Device1`
(`BluezMonitor::le_drop_count()`), and `unsubscribe()` sends `StopNotify` on both sources before
every rebuild.

Not settled: the exact `ReadValue` error on the GAP characteristic with only BR/EDR up, and the
fix itself on hardware (walk out until only `LE1` times out, come back, expect `LE link dropped`
then a new session and a popup).

### 2026-09-20 - `--bt-setup` kept printing the experimental step inside Flatpak (#206)

Reported in #206: the drop-in `--bt-setup` printed was applied, `bluetooth` restarted, and the
rerun under `flatpak run --command=tether com.tether.desktop --bt-setup` listed the same step.

`bluetoothd` has no property for its own flags, so the check walked `/proc` for a process named
`bluetoothd` and looked for `-E` / `--experimental`. Flatpak gives the app its own PID namespace:

```
$ flatpak run --command=sh <app> -c 'ls /proc | grep -c "^[0-9]*$"'
4                       bwrap, sh, and the pipeline -- no bluetoothd, ever
```

So `experimental_api` was always false in the sandbox, and with nothing bonded yet to populate
`Bearer.LE1` the resolver had no second source and pinned `BearerApi::Absent`. The same false
negative hit every install method whenever the flag came from `main.conf` `Experimental = true`,
which is what the non-systemd branch of the step itself tells users to do -- the open item left
in the 2026-09-10 entry above.

`bluetoothd` answers the question on its own bus. `btd_adv_monitor_manager_create()` runs only
inside `if (g_dbus_get_flags() & G_DBUS_FLAG_ENABLE_EXPERIMENTAL)` (`src/adapter.c`, 5.87 and
master), so `org.bluez.AdvertisementMonitorManager1` on an adapter object exists only with the
experimental API on. It arrives in the `GetManagedObjects` reply already parsed, so detection
costs no extra call, no cache, and no `/proc`, and `scan_bluetoothd_cmdline()` is gone.

```
$ busctl --system introspect org.bluez /org/bluez/hci0 | grep AdvertisementMonitor
org.bluez.AdvertisementMonitorManager1  interface
```

Verified on this machine against the live bus: `experimental_api=1`, `bearer_api=Confirmed`,
only the class step left. The marker is a BlueZ implementation detail, not API: if a later
release creates the monitor manager unconditionally, this reads experimental where there is
none, and the check moves to `Adapter1.ConnectDevice`, which is `GDBUS_EXPERIMENTAL_ASYNC_METHOD`
and therefore absent from introspection without the flag.

Not verified here: the negative case, which needs `bluetoothd` restarted without the flag.
