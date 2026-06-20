# Tether Mail Extension — addons.thunderbird.net listing copy

## Summary (short description)

Automatically detects one-time passcodes (OTP / 2FA verification codes) in
incoming email and hands them to the local Tether daemon, so codes can be synced
to your phone or autofilled by the Tether browser extension.

---

## Description (full listing body)

**Tether Mail Extension** is part of [Tether](https://github.com/zackb/tether),
which bridges your iPhone to the Linux desktop. This add-on watches your inbox for
verification / login / 2FA codes and forwards the detected code to the Tether
daemon running on your machine.

It works entirely in the background. Once the add-on is installed and the Tether daemon is running,
OTP emails are captured automatically.

### How it works / where to find it

There is no UI surface to click. After installation the extension:

1. Connects to the local Tether daemon over native messaging on startup.
2. Watches your inbox for new mail using several mechanisms (polling every 15s for
   IMAP, plus new-mail and message-display events).
3. Pre-filters messages by subject (e.g. "verification", "your … code", "OTP",
   "security code") and sender (e.g. `noreply`, `security`, `verify`).
4. Extracts the most likely code from the matching email using a scoring heuristic
   that ignores years, prices, zip codes, and order/tracking numbers.
5. Sends the detected code to the Tether daemon, which can copy it to your
   clipboard, sync it to your iPhone, or make it available to the Tether browser
   extension for autofill.

To confirm it's running, open Thunderbird's **Tools → Developer Tools → Error
Console** (or the add-on's Browser Console) and look for the log line
`Tether Mail Extractor loaded in Thunderbird/Betterbird`.

### Required: the Tether native application

⚠️ **This add-on does nothing on its own.** It requires the local **Tether
daemon (`tetherd`)** to be installed and running. The add-on communicates with it
via native messaging (native host `com.tether.extension`); without it, detected
codes have nowhere to go.

Install the native app on Linux using one of:

- **Arch Linux (AUR):**
  ```bash
  yay -S tether
  ```
- **GitHub Releases** — download the `.deb`, `.rpm`, or `.tar.gz` for your distro
  from <https://github.com/zackb/tether/releases>.
- **Build from source:**
  ```bash
  git clone https://github.com/zackb/tether.git
  cd tether
  make release
  sudo make install
  ```
  `make install` also registers the native messaging host with Thunderbird so the
  add-on can reach the daemon.

The daemon (`tetherd`), will start on-demand when needed. Full documentation and
setup details are at <https://github.com/zackb/tether>.

---

## Screenshots to attach

The extension is background-only, so screenshots should demonstrate the
end-to-end flow rather than an in-app UI:

1. **Add-on installed** — the Tether Mail Extension enabled in Thunderbird's
   Add-ons Manager.
2. **Example OTP email** — a verification / 2FA email open in Thunderbird (use a
   redacted sample code).
3. **Code captured by Tether** — the detected code arriving in Tether, e.g.
   `tether` CLI output, the system clipboard, or the iPhone receiving it —
   showing the captured-and-forwarded result.

Branding assets available in the repo for the listing image/header:
`docs/img/tether_header.png`, `docs/img/tether-dark.svg`,
`docs/img/tether-light.svg`.
