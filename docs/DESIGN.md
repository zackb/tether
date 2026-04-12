# Tether

**Cross-Device Companion Integration for iPhone + Linux**
*Design Document · Draft 0.1*

---

## 1. Overview

Tether is an open-source companion device integration tool for iPhone and Linux, targeting Wayland compositors — initially Hyprland, but designed to run on any Wayland desktop. It is the missing equivalent of Apple's Continuity and AirDrop for people who have left the Apple ecosystem on the desktop but retained an iPhone.

The name captures the relationship well: a tether connects two things without merging them. It works as a verb ("tether your phone"), reads cleanly as a CLI tool (`tetherd`), and is not tied to any specific desktop environment.

**Scope of v1.0:**

- Shared clipboard between iPhone and Linux desktop
- TOTP/OTP vault with autofill in the browser via extension
- File transfer over local network
- Email OTP code detection and forwarding to browser

---

## 2. Problem Statement

macOS users enjoy a tightly integrated device ecosystem: Universal Clipboard syncs seamlessly between Mac and iPhone, SMS and email OTP codes appear automatically above the keyboard and autofill in Safari, and AirDrop makes file transfer invisible. None of this survives a migration to Linux.

The closest existing solution is KDE Connect, which runs on Hyprland but has persistent theming issues with qt6ct — specifically its QtQuick/QML interface bypasses the platform theme and queries KColorScheme directly. GSConnect, the GNOME Shell equivalent, is tied to the GNOME Shell process and does not run on Hyprland. Neither solution adequately addresses TOTP autofill or email OTP forwarding to the browser.

Apple's iOS SDK also severely limits what third-party apps can do. SMS inbox access is a private entitlement not granted to third-party developers. Background execution is constrained by `BGTaskScheduler`, which provides only periodic wake windows rather than real-time sync. Clipboard access via `UIPasteboard` works, but since iOS 14 every read triggers a user-visible banner. These constraints shape what Tether can and cannot promise on the iOS side.

---

## 3. Architecture

Tether has three components that communicate over an encrypted local network channel with an optional relay for out-of-network use.

| Component | Technology | Responsibility |
|---|---|---|
| `tetherd` | Rust or Go daemon | Linux-side coordinator; Wayland clipboard, file sync endpoint, local HTTP/Unix socket API |
| Browser Extension | WebExtension (FF + Chrome) | TOTP autofill, OTP detection, native messaging host bridge to tetherd |
| iPhone App | Swift / SwiftUI | Clipboard sync, TOTP vault, file sharing, `ASCredentialProviderExtension` |

All three components communicate via an encrypted local network channel. The daemon advertises itself via mDNS/Bonjour so the iPhone app can discover it without manual IP configuration. An optional relay server (self-hostable) allows sync when devices are on different networks.

```
iPhone App (Swift)
  ├── UIPasteboard sync  →──────────────────────────────┐
  ├── TOTP vault + ASCredentialProviderExtension        │
  └── File sharing (Bonjour/mDNS + TCP)                 │
          ↕ E2E encrypted · local network / relay       │
tetherd (Linux daemon)                                   │
  ├── Wayland clipboard (wl-clipboard / compositor IPC) ┘
  ├── File sync endpoint
  ├── TOTP vault sync
  └── Unix socket / local HTTP API
          ↕ native messaging
Browser Extension (WebExtension)
  ├── Native messaging host → tetherd
  ├── TOTP autofill
  └── Email OTP detection → clipboard → autofill
```

---

## 4. Components

### 4.1 tetherd — Linux Daemon

The daemon is the hub. It runs as a user-space process, started via `exec-once` in Hyprland config or a systemd user unit, and exposes a Unix socket for local IPC and a TCP endpoint for the iPhone app.

#### Wayland Clipboard

Wayland intentionally restricts clipboard access to the focused window. The daemon works around this by running with compositor-level privileges or by using Hyprland's IPC socket directly. On other compositors, it falls back to `wl-clipboard` (`wl-copy` / `wl-paste`) polled on a short interval.

- Watches clipboard for changes and pushes to iPhone app
- Receives clipboard content from iPhone app and writes via `wl-copy`
- Exposes clipboard state to browser extension via Unix socket

#### File Sync

- Serves a simple authenticated HTTP endpoint for file push/pull
- mDNS advertisement so iPhone app discovers it automatically
- Configurable receive directory

#### TOTP Vault

- Stores encrypted TOTP secrets, synced from iPhone app
- Generates current codes on request from browser extension
- Never stores secrets in plaintext; vault key derived from user passphrase

---

### 4.2 Browser Extension

A standard WebExtension compatible with Firefox and Chromium-based browsers. Communicates with `tetherd` via a native messaging host — a small binary installed alongside tetherd that the browser can launch and talk to over stdin/stdout.

#### TOTP Autofill

- Detects OTP input fields on page load
- Requests current TOTP code from tetherd via native messaging host
- Injects code into field; user can also trigger via keyboard shortcut or extension popup

#### Email OTP Detection

Rather than requiring a specific email client, the extension monitors the active tab if it is a webmail client, or listens for signals from the email client daemon integration.

- **Terminal clients (aerc, neomutt):** a filter/hook script detects OTP patterns in incoming mail and writes the code to a named pipe or tetherd's socket
- **Webmail:** extension content script scans new email notifications for OTP patterns
- Detected code is surfaced as an autofill suggestion on the matching domain

> ⚠ SMS-based OTP forwarding from iPhone is not possible. Apple's SMS inbox is inaccessible to third-party apps — this is a hard platform restriction, not a missing feature.

---

### 4.3 iPhone App

A native SwiftUI app targeting iOS 16+. Apple's sandbox and background execution restrictions shape what is possible, but the core features are achievable.

#### Clipboard Sync

The app reads from `UIPasteboard` and sends to tetherd when foregrounded. Since iOS 14 every clipboard read triggers a user-visible "Tether pasted from [app]" banner — this is unavoidable. Background sync uses `BGTaskScheduler` for periodic wake windows (roughly every 15 minutes), so clipboard sync is not real-time when backgrounded. Foregrounded sync is instant.

#### TOTP Vault + ASCredentialProviderExtension

This is the most valuable feature on the iOS side. iOS 18 introduced a public credential provider API (`ASCredentialProviderExtension`) that allows third-party apps to surface TOTP codes in the system autofill menu — the same one that appears above the keyboard when a code field is focused. Tether implements this extension to provide:

- One-tap OTP autofill in Safari and other iOS browsers
- Sync of TOTP secrets with tetherd so the same vault is available on both devices
- No dependency on iCloud or any Apple account for vault storage

The vault is encrypted with a user passphrase. Sync between the iPhone app and tetherd uses the same E2E encrypted channel as clipboard sync.

> ⚠ This is distinct from SMS OTP autofill. Tether's TOTP vault handles authenticator-app-style codes (TOTP). SMS codes remain unforwardable from iPhone to Linux.

#### File Sharing

- Discovers tetherd via mDNS/Bonjour on the local network
- Share sheet integration: share any file to Tether from any app
- Browse and pull files from the Linux machine

---

## 5. Explicit Non-Goals (v1.0)

- **SMS forwarding from iPhone** — not possible without private Apple entitlements
- **Notification mirroring from iPhone** — iOS 14+ blocks third-party apps from reading other apps' notifications
- **Screen mirroring or remote control**
- **Android support** — out of scope; KDE Connect already covers this well
- **Cloud relay in the default build** — self-host only for v1.0

---

## 6. Naming & Packaging

| | |
|---|---|
| Name | Tether |
| Daemon binary | `tetherd` |
| CLI | `tether` |
| Config | `~/.config/tether/tether.toml` |
| Primary packaging | AUR (Arch), daemon works on any Wayland Linux |
| iOS distribution | App Store (required for `ASCredentialProviderExtension`) |

---

## 7. Open Questions

- **Daemon language:** Rust gives a better AUR packaging story and aligns with the Hyprland ecosystem; Go is simpler for the network/IPC layer. Decision pending.
- **Relay design:** whether to build a minimal relay server or recommend users run their own ClipCascade instance for cross-network sync.
- **Email OTP detection scope:** aerc hook integration is clean and well-defined; webmail content script scanning is messier. May ship aerc-only first.
- **Vault sync conflict resolution:** if codes are added on both devices while offline, last-write-wins is simplest but could lose data.
- **Name conflict check:** verify `tether` is clear in the AUR before committing.
