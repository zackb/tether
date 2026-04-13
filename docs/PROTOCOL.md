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
