# Bluetooth: iPhone messages and notifications

Tether accesses SMS/iMessage and other apps' notifications via Bluetooth because iOS apps are
not allowed to access these. It presents the Linux machine to iOS as a paired
Bluetooth accessory. No iOS-side code is involved.

| Feature | Mechanism | Transport | Linux role |
|---|---|---|---|
| SMS / iMessage, read + send | **MAP** (Message Access Profile) via BlueZ `obexd` | BR/EDR | OBEX client |
| Contacts, for sender names | **PBAP** (Phonebook Access Profile) via `obexd` | BR/EDR | OBEX client |
| Notifications from any app | **ANCS** (Apple Notification Center Service) | BLE / GATT | GATT central |

The Tether iOS app is unrelated to these features and just keeps handling clipboard sync and file transfer over TCP + mTLS.

## Delivery modes

Not every machine can do all of it, so Tether resolves one of two modes from live capabilities.

- Full mode: MAP + PBAP + ANCS. Requires BR/EDR, LE, and advertising support, plus
  BlueZ >= 5.86 running with its experimental bearer API (org.bluez.Bearer.LE1).
- Compatibility mode: MAP + PBAP only. Messages and contacts work, notification mirroring does not (older BlueZ).

Run the probe to find out which applies:

```bash
./scripts/bt-probe.sh              # read-only
./scripts/bt-probe.sh --set-class  # also fixes the adapter Class of Device
```

## Setup requirements

Class of Device must be A/V Hands-Free (major 4, minor 8). iOS only offers the
"Show Message Notifications" and "Sync Contacts" permissions to a device presenting this
class. `sudo btmgmt class 4 8` sets it for now.

**It does not survive a `bluetoothd` restart**. bluetoothd rewrites the
class from its own default every time it starts. Already-granted session survives that, but a phone that has not yet granted the permissions will refuse MAP and PBAP with an OBEX error that seems like a missing service record rather than
like a permissions problem.
The packaged unit handles that:

```bash
sudo systemctl enable --now tether-btclass@hci0
```

`PartOf=bluetooth.service` re-runs it on every `bluetooth.service` restart. It writes the
class, reads it back, and retries for ten seconds.

**BlueZ needs the experimental bearer API** for ANCS. On Arch:

```bash
sudo mkdir -p /etc/systemd/system/bluetooth.service.d
printf '[Service]\nExecStart=\nExecStart=/usr/lib/bluetooth/bluetoothd --experimental\n' \
  | sudo tee /etc/systemd/system/bluetooth.service.d/experimental.conf
sudo systemctl daemon-reload && sudo systemctl restart bluetooth
```

The executable lives at `/usr/libexec/bluetooth/bluetoothd` on Debian based.

**`obexd` must be running** (user service `obex`) for MAP and PBAP. It is socket-activated under normal use.

## Permissions on the phone

After pairing, the iPhone offers "Show Message Notifications" and "Sync Contacts" under
Settings -> Bluetooth -> (i) for the computer's entry. Off by default.

- The toggles can take minutes to appear after paring, and appear only while the ANCS 
  advertisement is actively broadcasting. Failed MAP connection attempts alone do not surface them.
- Closing and reopening the entry's detail page refreshes what iOS shows.
- A failed pairing can leave two records for the same computer, the toggles may appear
  under either one. Check both, and delete both before retrying a clean pairing.

Without the relevant permission, MAP or PBAP is visible at the SDP level but rejects the
OBEX connection with `Forbidden` / `0x43`. That is a permissions state, not a pairing
failure (do not re-pair). A transport-level `Connection refused (111)` is different and
usually means another computer already owns the iPhone's single MAP session.

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

Start with the two read-only checks. `./scripts/bt-probe.sh` says what the hardware and the
stack can do. `tether --bt-connection` says what is actually up right now.

| Symptom | Cause | What to do |
|---|---|---|
| Messages and contacts worked, then stopped, and the error mentions a service record | `bluetoothd` restarted and reset the Class of Device | `sudo systemctl enable --now tether-btclass@hci0`, then re-pair if the phone dropped the bond |
| The phone never offers notifications / Sync Contacts | The class is wrong, or the ANCS advertisement is not running | Check for `class=ok` in `tether --bt-status`, can take minutes |
| MAP or PBAP reports `forbidden` | The matching toggle on the phone is off | Turn it on. This is not a pairing failure |
| MAP reports `busy`, or the transport says `Connection refused (111)` | Another computer holds the iPhone's single MAP session | Stop the other client |
| Pairing fails with `br-connection-key-missing` | A stale bond on one side, or the adapter is not `Pairable` | Delete the computer's entry on the phone (Forget This Device) and `tether --bt-unpair <addr>` locally, then pair again |
| The phone shows two entries for this computer | A failed pairing left both a Classic and an LE record | Delete both on the phone before retrying |
| LE never connects and the log repeats `org.bluez.Error.InProgress` | BlueZ is holding a connect operation that never completed | `sudo systemctl restart bluetooth`. Disconnecting the device does not clear it. With `tether-btclass@hci0` enabled the class survives the restart |
| Everything connects but `ancs_ready` stays false | Compatibility mode, or iOS has not authorized notification content yet | Check `Mode:` in `tether --bt-status`. In full mode the daemon retries, the first request returns `NotPermitted` until the prompt on the phone is approved |
| A group conversation cannot be replied to | Working as designed until the route is unambiguous | The thread's `reply_reason` says which condition failed |

### Reporting a problem

```bash
tether --bt-diagnostics
```

Prints the delivery mode, auth strategy, Bluetooth settings, current connection state, and timeline of recent link and pairing transitions.

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

### Group messages

Off by default, `group_messages_enabled` and
`ancs_content_enabled` in `~/.config/tether/bluetooth.json`, so group support cannot work without content mirroring.

MAP delivers a group message with one sender, no participant list and no conversation
identifier. The only other hint is to correlate Apple Messages ANCS notification:
its title is the sender, and its subtitle is either `To you & ...` for an unnamed group or
the group's name. Neither form contains a member list...

- Correlation is bounded to a 30-second window, and two notifications with the same
  text are refused instead of guessing, the wrong choice would put a message in the wrong conversation and send a reply at the wrong people.
- An unnamed group is repliable only when **every** participant name resolves to exactly one contact address. A name matching several contacts is refused.
- A named group stays read-only until we figure out the member list in `~/.config/tether/groups.json`.
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

### PBAP

`Select("int", "pb")` followed by `PullAll` with `Format: vcard30` and `MaxCount` returned 456 contacts on the first attempt.

The transfer object disappears from D-Bus the moment it finishes, so a vanished object
is a normal terminal state rather than a failure (file may still take a bit to appear afterwards),
which is why the pull waits instead of giving up. Contacts are staged in `$XDG_RUNTIME_DIR` rather than `/tmp`, since a
phonebook is personal data, and the cache and journal are both written mode 0600.

***THIS HAS TO CHANGE BEFORE PUBLIC RELEASE.***

## Credits

[BlueFerry](https://github.com/erikwb/blueferry) (Erik Bourget and contributors), `PROTOCOL.md` records the findings this implementation relies on.
Tether's Bluetooth support is an independent implementation written against those published
findings, Apple's ANCS specification, the Bluetooth SIG MAP and PBAP specifications, and
the BlueZ D-Bus API.
