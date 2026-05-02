![tether](docs/img/tether_header.png)

# Tether

> Bridge your iPhone to the Linux Wayland desktop

**Tether** is an open-source companion device integration suite that bridges iOS (iPhone/iPad) and the Linux Wayland desktop. If you've migrated away from macOS but still carry an iPhone, Tether provides the missing ecosystem integration.
A self-hosted alternative to Apple's Continuity and Universal Clipboard.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

[![Platform: Linux](https://img.shields.io/badge/Platform-Linux%20Wayland-blue.svg)](https://wayland.freedesktop.org/)

[![iOS: 16+](https://img.shields.io/badge/iOS-16%2B-purple.svg)](https://apps.apple.com/us/app/tether-linux-companion/id6762097135)

[![Download on the App Store](https://developer.apple.com/assets/elements/badges/download-on-the-app-store.svg)](https://apps.apple.com/us/app/tether-linux-companion/id6762097135)

## Features

| Feature | Status |
|---------|--------|
| **Clipboard Sync** | ✅ Stable |
| **File Transfer** | ✅ Stable |
| **iOS App** | ✅ Stable |
| **Browser Extension** | 🔄 In Progress |
| **Mail Extension** | 🔄 In Progress |
| **TOTP/OTP Vault** | 🗓️ Planning |

### Clipboard Sync
Seamlessly share your Wayland compositor (`wlr-data-control`) clipboard over the Tether network. Text copied on your Linux desktop appears instantly on your iPhone, and vice versa.

### File Transfer
Drag and drop files from Linux directly into the iPhone app, or receive files automatically deposited to your `$XDG_DOWNLOAD_DIR`.

### Device Pairing
Securely pair devices using **Mutually Authenticated TLS (mTLS)**, restricting TCP traffic securely using X.509 RSA certificates.

### Browser & Mail Extensions (WIP)
A unified WebExtension that works across Thunderbird/Betterbird and Firefox/Chromium browsers:

- **Thunderbird/Betterbird:** Detects OTP codes in incoming emails (verification codes, 2FA messages) and copies them to the clipboard or sends them to the Tether daemon for vault storage
- **Firefox/Chromium:** Autofills OTP codes into login forms by retrieving secrets from the Tether vault, with one-click verification for sites using TOTP-based 2FA

The extension communicates with `tetherd` via native messaging. This allows users to autofill OTP codes into websites when the email arrives. 

## Architecture

Tether uses a multi-component architecture:

1. **`tetherd` (Linux Daemon)** — A background C++ process built on `epoll` and Wayland protocols via `hyprwayland-scanner`. Anchors the Linux clipboard and broadcasts events over UNIX Domain Sockets (`$XDG_RUNTIME_DIR/tether/tetherd.sock`). Network traffic securely traverses local TCP via OpenSSL.

2. **`tether` (CLI)** — A lightweight C++ CLI program to communicate with the daemon. This also allows the WebExtension to interface with the daemon via native messaging.

3. **`tether-gtk` (GTK App)** — A native GTK4 application for Linux that provides a graphical interface to manage devices, send files, trigger clipboard sync, and monitor connection status.

4. **iPhone App** — Native SwiftUI iOS 16+ app that discovers the daemon via Bonjour/mDNS, utilizing Apple's `Network.framework` for secure TLS negotiation.

5. **Browser/Mail Extension (WIP)** — WebExtension that interfaces with the daemon via native messaging. Use with Thunderbird/Betterbird and Firefox or Chromium-based browsers.

## Requirements

### Linux
- **Wayland compositor** with `wlr-data-control` protocol (Hyprland, Sway, etc.)
- **Linux 5.15+** with glibc
- **Build tools:** CMake, Ninja, pkg-config

### Dependencies
- `wayland-client`
- `openssl`
- `pkg-config`
- `ninja`
- `gtk4` (for `tether-gtk`)
- `nlohmann-json`
- `glib2`

### iOS
- iOS 16+
- Get the app: [Tether - Linux Companion](https://apps.apple.com/us/app/tether-linux-companion/id6762097135)

## Installation

### Build from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/tether.git
cd tether

# Build with CMake presets
make debug

# Run the daemon
make run-daemon
```

The daemon uses an advisory lock (`$XDG_RUNTIME_DIR/tether/tetherd.lock`) ensuring only one instance controls the Wayland connection at a time.

## Quick Start

1. **Build the project:**
   ```bash
   make debug
   ```

2. **Start the daemon:** (optional, the GTK app will auto-launch it if not running)
   ```bash
   make run-daemon
   ```

3. **Launch the GTK app** (optional, for GUI-based device management):
   ```bash
   make run-gtk
   ```

4. **Pair your devices** (select the auto-discovered iPhone from the GTK app or CLI):
   ```bash
   ./build/debug/tether pair
   ```

5. **Start syncing** clipboard and transferring files

## Roadmap

- [x] Complete iOS app development
- [x] Add end-to-end encryption for file transfers
- [ ] Implement TOTP/OTP vault with Safari autofill
- [ ] Release browser/mail extension for Thunderbird and Firefox
- [ ] Explore macOS support

## Contributing

Contributions are welcome! Please read our contributing guidelines before submitting PRs.

## License

Tether is licensed under the [MIT License](LICENSE).
