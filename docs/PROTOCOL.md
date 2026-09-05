# Tether Communication Protocol

The `tetherd` daemon communicates with its clients (the `tether` CLI, the browser extension via native messaging, and the iPhone App via TCP) using a simple, unified protocol over standard sockets.

## Protocol Structure

All communication happens via **newline-delimited JSON**. Every individual command or broadcast must:
1. Be a valid JSON object.
2. Terminate with a literal `\n` character.

If a payload arrives that cannot be parsed as JSON, the daemon will gracefully ignore it. After successfully processing a message, the server currently sends an ad-hoc text response `OK\n`, though standard JSON responses may replace this over time.

---

## 1. Clipboard Sync

The clipboard capabilities allow seamless copying and pasting between the Host (Linux / Wayland) and Clients (iPhone / Browser).

### `clipboard_set` (Client -> Daemon)

**Description**: Sent by the client when it wants to update the Host's Wayland clipboard.

**Payload**:
```json
{
  "command": "clipboard_set",
  "content": "The text you want to place directly onto the wayland clipboard."
}
```

**Daemon Behavior**: The daemon writes the `content` out to the immediate active Wayland session by creating a `wlr_data_control_source_v1` offer.

---

### `clipboard_updated` (Daemon -> Clients)

**Description**: Broadcasted from the daemon to *all connected active clients* when the Host's clipboard text natively changes (e.g., when the user highlights and presses `Ctrl-C` in a desktop app).

**Payload**:
```json
{
  "command": "clipboard_updated",
  "content": "The newly detected clipboard text."
}
```

**Daemon Behavior**: Fired asynchronously whenever the `wlr_data_control_device` `setDataOffer` informs the daemon that the primary clipboard selection has rotated.

---

## 2. File Transfers

Tether supports streaming large binary files seamlessly through JSON using Base64 chunks to adhere to extension native messaging payload restrictions.

### `file_start`

**Description**: Announces an incoming binary transmission.

**Direction**:
- `Client -> Daemon` when a remote device uploads into the Linux host.
- `Daemon -> Client` when a local Unix client asks `tetherd` to push a file to a connected mobile app.

**Payload**:
```json
{
  "command": "file_start",
  "filename": "image.png",
  "size": 2048500,
  "transfer_id": "unique_string"
}
```

### `file_chunk`

**Description**: Sends a sequential Base64 chunk of the file data.

**Direction**:
- `Client -> Daemon` for mobile-to-Linux uploads.
- `Daemon -> Client` for Linux-to-mobile sends.

**Payload**:
```json
{
  "command": "file_chunk",
  "transfer_id": "unique_string",
  "chunk_index": 0,
  "data": "iVBORw0KGgoAAAANSUhEUgAA..."
}
```

### `file_end`

**Description**: Marks the end of the streamed file.

**Direction**:
- `Client -> Daemon` for mobile-to-Linux uploads.
- `Daemon -> Client` for Linux-to-mobile sends.

**Payload**:
```json
{
  "command": "file_end",
  "transfer_id": "unique_string"
}
```

**Response (Daemon -> Client):**
```json
{
  "command": "file_status",
  "transfer_id": "unique_string",
  "status": "success"
}
```

For local Unix clients, `tetherd` returns `file_status` once the stream has been forwarded to at least one connected TCP client. If no mobile client is connected, the daemon returns:

```json
{
  "command": "error",
  "message": "no_connected_mobile_client"
}
```

---

## 3. Device Pairing (TLS + fingerprint pinning)

The `tetherd` daemon binds `[::]:5134` dual-stack (falling back to `0.0.0.0:5134` where IPv6 is unavailable) and speaks TLS 1.2. Both sides present a self-signed X.509 certificate; there is no CA in this system, so certificates are **not** validated against any trust anchor.

Access control is trust-on-first-use fingerprint pinning. After the handshake the daemon takes the SHA-256 fingerprint of the peer's certificate and looks it up in `known_hosts.json`. A client whose fingerprint is not pinned may only send `pair_request` — any other command is answered with `{"command":"error","message":"unauthorized"}`.

TLS still verifies the peer's `CertificateVerify` signature, so a pinned fingerprint proves the peer holds the matching private key. An attacker on the same network cannot impersonate a paired device without that key, but they can open a connection and sit at the pairing prompt.

Every node is a peer. A PC, iPhone or iPad all listen and all dial, and one certificate serves both roles, so a node's fingerprint is the same whether it opened the connection or accepted it. Pairing is therefore direction-agnostic but never symmetric in authority:

- The **dialling** node sends `pair_request` as soon as its handshake completes. Dialling is its own consent, so it prompts nobody locally.
- The **receiving** node prompts its user (the desktop dialog, or `tether --accept <fingerprint>`).
- Only the receiver's approval settles it. It replies `pair_accepted`, and **both** sides then write the other's fingerprint to their own `known_hosts.json`.

A node must never pin a peer on local assertion alone — a UI must not report a device paired or connected until `pair_accepted` has crossed the wire and the trust record exists.

### Reconnecting

A node dials a peer it already trusts on its own, whenever mDNS resolves one whose `fp=` is in `known_hosts.json`.

Only the node with the **lower** fingerprint dials. Every node both listens and browses, so an unconditional dial leaves each side holding two sessions with the same peer, and every broadcast goes out twice. The comparison is reciprocal, so exactly one of any two nodes dials and no negotiation is needed.

The iPhone app dials on launch and is therefore reachable either way.

### `pair_request` (Untrusted Client -> Daemon)

**Description**: Emitted natively by a new client over standard TLS to gracefully present its identity and X.509 fingerprint. The Daemon immediately intercepts the payload, extracts the fingerprint natively from the `SSL*` pipe, and flags it locally as "Pending Authentication". Anything other than `pair_request` results in the TLS socket securely disconnecting.

**Payload**:
```json
{
  "command": "pair_request",
  "device_name": "My iPhone"
}
```

### `pair_pending` (Daemon -> Untrusted Client)

**Description**: Emitted immediately by the Daemon natively over the pending TLS pipe confirming the Request is actively intercepting user approval on local stdout.

**Payload**:
```json
{
  "command": "pair_pending"
}
```

### `pair_accepted` (Approving Node -> Requesting Node)

**Description**: The approver's verdict, sent on the same TLS session once its user accepts. The requesting node pins the approver's fingerprint on receipt and the session is promoted in place — no reconnect.

**Payload**:
```json
{
  "command": "pair_accepted"
}
```

Once paired (via `tether --accept <fingerprint>`, the desktop dialog, or the peer's `pair_accepted`), the fingerprint is written to `known_hosts.json`. Later connections presenting that certificate are treated as paired and may issue any command.

---

## 4. Local Daemon Control (UNIX Socket)

The `tetherd.sock` UNIX socket is strict local IPC used by local clients (such as the GTK frontend or CLI) wanting to control the daemon's behavior without executing heavy operations directly.

### `discover`
Instructs the daemon to perform a synchronous mDNS scan (3 seconds) in a background thread for remote tether services.
**Payload**: `{"command": "discover"}`
**Response**: The daemon broadcasts a `discovery_result` payload asynchronously containing an array of active `devices`.

### `pair_request` (Local Client -> Daemon)
Instructs the daemon to dial a peer and hold the resulting TLS session itself, sending `pair_request` on it as described in §3. `device_name` is the peer's advertised mDNS name, used to label the trust record; it is optional and falls back to the address.
**Payload**: `{"command": "pair_request", "host": "192.168.1.5", "port": 5134, "device_name": "workstation"}`
**Response**: The daemon broadcasts `pair_outbound_pending` while the peer's user decides, then either `pair_accepted` (with `connected: true`) or `pair_rejected`. A dial to a peer the daemon already has a session with is a no-op.

### `accept_device` (Local Client -> Daemon)
Trusts a pending pair request by moving the target fingerprint into the daemon's internal secure `known_hosts.json`.
**Payload**: `{"command": "accept_device", "fingerprint": "12:aa:bb:cc..."}`
**Response**: The daemon promotes any matching live TLS session, notifies the phone and local subscribers,
then replies with `{"command":"accept_device_result","accepted":true,"connected":true}`. `connected` is false
when the trust record was saved for a device that is no longer connected.

### `forget_device` (Local Client -> Daemon)
Removes a fingerprint from `known_hosts.json` and closes any live session holding it.
**Payload**: `{"command": "forget_device", "fingerprint": "12:aa:bb:cc..."}`
**Response**: `{"command":"forget_device_result","fingerprint":"...","forgotten":true}`, also broadcast
to local subscribers. `forgotten` is false when the fingerprint was not pinned. Both ends must forget
a peer: a node that still trusts it will keep dialling it (see §3).

### `send_file` (Local Client -> Daemon)
Offloads an entire file transfer to the daemon. The daemon spawns a thread to read the local filesystem and pushes the chunks sequence securely.
**Payload**: `{"command": "send_file", "path": "/absolute/path/to/my_video.mp4"}`
**Response**: `{"command": "file_send_complete", "success": true, "message": "Sent my_video.mp4"}`

---

## 5. OTP Vault (Extension Integration)

Tether uses the local UNIX socket to coordinate OTP syncing between mail clients (Thunderbird/Betterbird) and web browsers (Firefox/Chrome). The daemon acts as an ephemeral secure vault for these codes.

The daemon also fills the vault itself: incoming Bluetooth messages (MAP) and iPhone notifications (ANCS) are scanned for OTP codes, and a hit is pushed to local subscribers as `otp_available` exactly as a `new_otp` from the mail extension would be. The same code arriving twice (once over MAP, once over ANCS) is published only once.

### `new_otp` (Mail Extension -> Daemon)
**Description**: Sent by the mail extension via native messaging when an OTP code is scraped from a new email.
**Payload**:
```json
{
  "command": "new_otp",
  "otp": "123456",
  "source": "Your Login Code"
}
```
**Response**: `{"status": "ok"}`

### `request_otp` (Browser Extension -> Daemon)
**Description**: Sent by the browser extension via native messaging when a 2FA/OTP input field is detected natively in the DOM.
**Payload**:
```json
{
  "command": "request_otp",
  "url": "example.com"
}
```
**Response**: 
```json
{
  "command": "otp_available",
  "otp": "123456"
}
```

---

## 6. Bluetooth: iPhone Messages and Notifications

These commands ride the same UNIX socket and the same newline-delimited JSON. They are
local-only: nothing here is reachable over TCP, because everything here is personal data.

Two transports sit behind them, and they fail independently: MAP and PBAP are OBEX
sessions over BR/EDR, while ANCS is a GATT client over BLE. See [BLUETOOTH.md](BLUETOOTH.md) for how they are set up.

Some commands answer on the requesting socket, and some broadcast to every local
subscriber. Anything a second client would want to see is broadcast.

### Adapter and pairing

#### `bt_status` (Client -> Daemon, answered directly)
Reports adapter capability and the resolved delivery mode.
**Payload**: `{"command": "bt_status"}`
**Response**: `available`, `capability` (`mode` is `full` or `compatibility`, plus
`bearer_api`, `powered`, `le_central`, `le_peripheral`, `advertising`, `class_ok`, and a
`reasons` array naming anything missing), and an `adapters` array.

#### `bt_list_devices` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_list_devices"}`
**Response**: `{"command": "bt_devices", "devices": [...]}`, each entry carrying address,
name, `paired`, `bonded`, `connected`, `le_bearer`, and whether the device advertises
`map`, `pbap`, and `ancs`.

#### `bt_pair` / `bt_unpair` (Client -> Daemon, broadcast)
**Payload**: `{"command": "bt_pair", "address": "AA:BB:CC:DD:EE:FF"}`
Runs asynchronously and reports through `bt_pair_progress` events, then one
`bt_pair_result` (or `bt_unpair_result`). Only one pairing transaction runs at a time; a
second returns `{"status": "busy"}`. A successful pair selects the device and starts
supervision.

#### `bt_set_device` (Client -> Daemon)
**Payload**: `{"command": "bt_set_device", "address": "AA:BB:CC:DD:EE:FF"}`
Points supervision at an already-bonded phone.

#### `bt_pair_progress` / `bt_pair_result` (Daemon -> Clients)
`bt_pair_progress` carries `step` and `detail` for display during the transaction.
`bt_pair_result` carries `success`, `status`, `message`, and `dual_bond` — the last being
whether the bond covers LE as well as BR/EDR, which is what decides if ANCS is reachable.

### Connection state

#### `bt_connection` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_connection"}`
Returns the same object the daemon broadcasts as `bt_connection_changed`.

#### `bt_connection_changed` (Daemon -> Clients)
Broadcast when the published state actually changes, not on a timer.

```json
{
  "command": "bt_connection_changed",
  "device_present": true,
  "device_paired": true,
  "classic_connected": true,
  "le_available": true,
  "le_connected": false,
  "map_open": true,
  "pbap_open": true,
  "map_error": "none",
  "pbap_error": "none",
  "ancs_ready": false,
  "link_reason": "Connected. Bringing up the LE link for notifications...",
  "profile_reason": "Messages and contacts are connected."
}
```

The two `*_reason` strings are written for display (permission is off or a transport that is busy). 
`map_error` and `pbap_error` name the classification: `forbidden` means the toggle on the phone is off,
`busy` means another computer holds the phone's single MAP session.

### Messages

#### `bt_list_threads` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_list_threads"}`
**Response**: `{"command": "bt_threads", "threads": [...]}`. Each thread carries `thread`
(the key), `name`, `address`, `preview`, `timestamp`, `unread`, `count`, `group`, and
`repliable`. Group threads add `reply_status` and, when they cannot be replied to, a
`reply_reason` written for display.

Thread keys are derived from the normalized peer address — `tel:+15035550101` for a phone
number, the lowercased address for an Apple ID — because MAP provides no conversation
identifier. Group keys are `group:name:<slug>` or `group:members:<slugs>`.

#### `bt_list_messages` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_list_messages", "thread": "tel:+15035550101"}`
**Response**: `{"command": "bt_messages", "thread": ..., "messages": [...]}` with `handle`,
`body`, `timestamp`, `outgoing`, `read`, and `folder` per message.

#### `bt_list_contacts` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_list_contacts", "query": "ada", "limit": 100}`
Both fields are optional; an empty or absent `query` matches every contact, and `limit`
defaults to 100.
**Response**: `{"command": "bt_contacts", "query": ..., "contacts": [...]}`. Each contact
carries `name` and `addresses`, namespaced exactly as thread keys are
(`tel:+15035550101`, `email:ada@example.com`), so an address can be handed straight back
as the `thread` of a `bt_send_message`. Contacts with no usable address are omitted.

Matching is a case- and accent-insensitive substring over the name and every address,
with numbers matched in normalized form too, so `5551234567` finds a contact stored as
`+1 (555) 123-4567`. This is the only way to read the phonebook: the cache on disk is
encrypted and only `tetherd` holds the key.

#### `bt_mark_read` (Client -> Daemon, broadcast)
**Payload**: `{"command": "bt_mark_read", "handle": "...", "read": true}`
Writes through to the phone over OBEX, so it answers asynchronously with a
`bt_message_read` event carrying `success` and, on failure, `message`.

#### `bt_send_message` (Client -> Daemon, broadcast)
**Payload**: `{"command": "bt_send_message", "thread": "tel:+15035550101", "body": "on my way"}`
Builds a bMessage and pushes it to the phone's outbox. Answers asynchronously with
`bt_send_result` (`success`, and `message` when it failed).

Recipients are validated before they are interpolated into the bMessage: an address
containing CR, LF, or a vCard delimiter is rejected rather than escaped, because such an
address could otherwise add a recipient and deliver the message to someone else. Body lines
beginning with a bMessage structural token are byte-stuffed.

A successful send also broadcasts a `bt_message` event. That is the only record the send
happened — the iPhone's MAP sent folder stays empty and no outgoing notification arrives.

#### `bt_message` (Daemon -> Clients)
Broadcast once per newly observed message, incoming or locally sent.

### Notifications

#### `bt_list_notifications` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_list_notifications"}`
**Response**: `{"command": "bt_notifications", "notifications": [...]}` with `uid`,
`app_id`, `app_name`, `title`, `subtitle`, `body`, `category`, `timestamp`, and which
actions the notification offers.

`app_name` is the app's display name, resolved over ANCS `GetAppAttributes` and cached
per bundle id for the session. The first notification from an app carries a name derived
from its bundle id (`com.burbn.instagram` -> `Instagram`) and is corrected in place once
the phone answers.

Titles, subtitles and bodies are present when notification content mirroring is enabled,
which is the default -- the iPhone's own *Settings > Bluetooth > (i) > Show Message
Notifications* toggle is the consent gate. With it off, only `app_id` and `app_name` are
populated. Messages notifications (`com.apple.MobileSMS`) are retained but never raise a
desktop popup, since MAP already delivers those with working read state.

Since 0.2.24 `bt_status` also carries `version`, the daemon's own `TETHER_VERSION`. A
client that finds the field absent, or holding a version other than its own, is talking to
a `tetherd` left running across a package upgrade.

#### `bt_set_ancs_content` (Client -> Daemon, broadcast)
**Payload**: `{"command": "bt_set_ancs_content", "enabled": true}`
Turns notification content mirroring on or off. Persists to
`ancs_content_enabled` in `$XDG_CONFIG_HOME/tether/bluetooth.json`, applies to the running ANCS
client without a restart, and answers with a fresh `bt_status`. `bt_status` carries
`ancs_enabled` and `ancs_content_enabled` so a client can render the current state.

#### `bt_set_retention` (Client -> Daemon, broadcast)
**Payload**: `{"command": "bt_set_retention", "retention": "encrypted"}`
One of `encrypted` (the default), `plaintext`, or `none`. Persists to `retention` in
`$XDG_CONFIG_HOME/tether/bluetooth.json` and answers with a fresh `bt_status`.

Changing the mode moves what is already stored to the path the new mode uses; `none`
deletes the message journal and the contact cache outright. `bt_status` carries
`retention` alongside `retention_ready`, which is false when the mode is `encrypted` and
the wallet has no key to offer yet — the link stays up in that state, but nothing is
retained or replayed. See `BLUETOOTH.md` for the on-disk format.

#### `bt_notification_action` (Client -> Daemon, broadcast)
**Payload**: `{"command": "bt_notification_action", "uid": 42, "action": "positive"}`
ANCS offers positive and negative actions only — there is no free-text reply on a mirrored
notification. Answers with `bt_notification_action_result`.

### Diagnostics

#### `bt_diagnostics` (Client -> Daemon, answered directly)
**Payload**: `{"command": "bt_diagnostics"}`
Returns a report intended to be pasted into a bug report: version, auth strategy, the
Bluetooth settings in effect, the current capability and connection state, and an ordered
timeline of recent link and pairing transitions stamped in milliseconds.

The daemon redacts before answering. Bluetooth addresses, phone numbers, email addresses,
and home and runtime directories become numbered placeholders — `<address-1>` recurs for
one device, so a reader can still follow which device a line refers to without learning
which device it is. Message bodies, contact names, and notification content are dropped
outright rather than redacted, and message and notification events never enter the timeline
in the first place.
