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
