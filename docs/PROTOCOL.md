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

## 3. Device Pairing (mTLS)

Because the `tetherd` daemon natively binds to `0.0.0.0:5134` over TCP, it explicitly employs OpenSSL **Mutually Authenticated TLS (mTLS)** to natively reject malicious local-network payloads. 

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

Once explicitly paired (via `tether --accept <fingerprint>`), native mTLS fingerprint-matching activates permanently, unblocking future payload operations instantly!

---

## 4. Local Daemon Control (UNIX Socket)

The `tetherd.sock` UNIX socket is strict local IPC used by local clients (such as the GTK frontend or CLI) wanting to control the daemon's behavior without executing heavy operations directly.

### `discover`
Instructs the daemon to perform a synchronous mDNS scan (3 seconds) in a background thread for remote tether services.
**Payload**: `{"command": "discover"}`
**Response**: The daemon broadcasts a `discovery_result` payload asynchronously containing an array of active `devices`.

### `pair_request` (Local Client -> Daemon)
Instructs the daemon to asynchronously reach out to a specific host to initiate an mTLS pairing request.
**Payload**: `{"command": "pair_request", "host": "192.168.1.5", "port": 5134}`

### `accept_device` (Local Client -> Daemon)
Trusts a pending pair request by moving the target fingerprint into the daemon's internal secure `known_hosts.json`.
**Payload**: `{"command": "accept_device", "fingerprint": "12:aa:bb:cc..."}`
**Response**: The daemon synchronously broadcasts `pair_accepted` with the `fingerprint` and resolved `device_name`.

### `send_file` (Local Client -> Daemon)
Offloads an entire file transfer to the daemon. The daemon spawns a thread to read the local filesystem and pushes the chunks sequence securely.
**Payload**: `{"command": "send_file", "path": "/absolute/path/to/my_video.mp4"}`
**Response**: `{"command": "file_send_complete", "success": true, "message": "Sent my_video.mp4"}`

---

## 5. OTP Vault (Extension Integration)

Tether uses the local UNIX socket to coordinate OTP syncing between mail clients (Thunderbird/Betterbird) and web browsers (Firefox/Chrome). The daemon acts as an ephemeral secure vault for these codes.

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

Bodies are present only when notification content mirroring is enabled; by default the
daemon requests the owning app and nothing more. Messages notifications
(`com.apple.MobileSMS`) are retained but never raise a desktop popup, since MAP already
delivers those with working read state.

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
