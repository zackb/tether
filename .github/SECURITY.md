# Security Policy

Tether sits between your iPhone and your Linux desktop, and it handles things
that must be protected: device certificates, message contents, notification text, and
one-time passcodes. Security reports are welcome and taken seriously.

## Reporting a vulnerability

**Report privately, through GitHub:**

<https://github.com/zackb/tether/security/advisories/new>

That opens a private thread visible only to you and the maintainer. Please do
not open a public issue, comment on a pull request, or post details anywhere
public before a fix is out. Tether users run the daemon on their own machines,
and a public report is a working exploit until they upgrade.

A good report includes:

- The version (`tether --version`) and how it was installed (AUR, `.deb`,
  `.rpm`, source).
- Distro and Wayland compositor. For Bluetooth issues,
  `tether --bt-diagnostics` prints a redacted report that covers the
  adapter, BlueZ, and connection state.
- Steps to reproduce, and what an attacker gains.

Proof-of-concept code helps. So does a guess at the fix, but it is not
required.

## What to expect

Tether is maintained by one person in their spare time, so these are
intentions, not guarantees:

- Acknowledgement within a few days.
- An assessment — whether it is a vulnerability, and how severe — within about
  two weeks.
- A fix in a tagged release, with a published advisory.
- Credit in the advisory, unless you would rather not be named.

If a report turns out not to be a security issue, it gets moved to a normal
issue rather than dropped.

## Supported versions

Tether is pre-1.0 and moves fast. Only the latest tagged release receives
security fixes; there are no backports to older tags. Fixes ship as a new
release, which the distro packages follow.

Check what you are running with `tether --version` and compare against the
[latest release](https://github.com/zackb/tether/releases/latest).

## Scope

The parts of Tether where a bug is a security bug:

- **Pairing and TLS.** Certificate and key handling
  (`cert.pem`, `key.pem`, `known_hosts.json` under `$XDG_CONFIG_HOME/tether`,
  `~/.config/tether` by default), the pairing
  approval flow, and anything that lets an unpaired peer be trusted, or a
  paired peer be impersonated.
- **Bluetooth.** ANCS notification and MAP message handling, and the parsers
  behind them. Message and contact data arrives from a device over the air —
  memory-safety and injection bugs there matter.
- **Native messaging.** The channel between the `tether` CLI and the
  browser/mail extension, and any path by which a web page reaches daemon
  state it should not.
- **OTP handling.** Passcodes reaching the wrong origin, the wrong tab, or a
  field the user cannot see.
- **File transfer.** Writes into `$XDG_DOWNLOAD_DIR` — path traversal, symlink
  attacks, or unbounded writes from a hostile sender.
- **Local boundaries.** Permissions on stored secrets, and daemon IPC
  reachable by other users on the same machine.

Generally out of scope:

- Attacks that require an already-compromised desktop session, or root.
- The system changes `tether --bt-setup` prints. Tether deliberately does not
  apply them; enabling the BlueZ experimental bearer API changes how the
  machine behaves over Bluetooth, and that is the user's decision to make.
- Missing hardening with no demonstrated exploit path.
- Bugs in BlueZ, OpenSSL, GTK, Avahi, or iOS. Please report those upstream —
  though if Tether uses them in a way that creates a problem, that is in scope.
- Social engineering, physical access, and scanner output with no impact shown.

If you are unsure whether something is in scope, report it.

## Disclosure

Disclosure is coordinated. The fix lands first, the advisory is published with
the release that carries it, and you are asked to hold public detail until
then. If a fix is taking too long, 90 days from the report is a reasonable
point to publish regardless — tell me if you intend to.
