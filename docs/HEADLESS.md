# Tether on a machine with no desktop

Tether is built for a Wayland desktop, but the daemon does not need one. On a
server, a Raspberry Pi, or a machine you only ever reach over SSH, `tetherd`
still speaks to the iPhone over Wi-Fi and everything except clipboard sync
works. This is how to set that up and drive it from the CLI.

What you get, and what you do not:

| Feature | Headless |
|---|---|
| File transfer, both directions | ✅ |
| OTP handling and the vault | ✅ |
| Pairing, device management, status | ✅ |
| Messages, notifications, calls | ✅ with a Bluetooth adapter the machine can see |
| Clipboard sync | ❌ needs a Wayland session with `wlr-data-control` |

## Container deployment

For a non-root Docker image, Compose example and persistent mounts, see
[CONTAINER.md](CONTAINER.md). It reuses host BlueZ/Avahi and runs its own session
bus and OBEX daemon. It is CLI-driven, not a web UI, and Bluetooth messaging
still requires a nearby phone.

## Run the daemon under systemd

The distro packages install a `tetherd.service` user unit into
`/usr/lib/systemd/user`, so there is nothing to write. Enable it:

```bash
systemctl --user daemon-reload
systemctl --user enable --now tetherd.service
loginctl enable-linger $USER      # or the daemon dies with your SSH session
```

`loginctl enable-linger` is the part people miss. Without it systemd tears the
user session down when your last shell exits, and the daemon goes with it.

A portable build (an AppImage, or a tree you built yourself) has no package to
install that unit, so the CLI writes one pointed at wherever that build lives:

```bash
tether service  # writes ~/.config/systemd/user/tetherd.service
```

It never enables or starts anything: it prints the commands above and leaves the
decision to you. `tether --uninstall-service` takes the unit back.

Note the order. A client starts `tetherd` on its own when one is not already
running, so if you have used the CLI at all there is probably a daemon up
already, holding the port this unit wants. Enabling the unit takes it over.
Once the unit is enabled, clients stop spawning their own and leave the daemon
to systemd.

## Without systemd

On Artix, Void, Devuan, or Gentoo with OpenRC there is no user unit to enable,
and `tether service` says so. `tetherd` itself does not need systemd, but
whatever starts it has to provide three things:

- **Your user.** Never root: the keys and pairings live in your home directory.
- **A runtime directory.** Clients find the daemon's socket in
  `$XDG_RUNTIME_DIR/tether`, or `/run/user/<uid>/tether` when the variable is
  unset, as it is for anything an init system starts. elogind, turnstile, and
  pam_rundir create that directory at login and remove it at your last logout,
  so check it survives a logout before relying on a daemon that outlives you.
- **A session bus.** Messages and contacts (`obexd`), the secret store, and
  notifications live on it. With no desktop session, start the daemon inside
  one: `dbus-run-session tetherd`.

On a desktop, start it from the compositor instead (`exec-once = tetherd` in
Hyprland, `exec tetherd` in Sway), or do nothing: a client starts `tetherd`
when none is running.

## Pair the iPhone

The daemon advertises itself over mDNS and the iOS app finds it. Both ends have
to accept, and on the desktop that prompt is a GTK window you do not have here,
so accept it from the shell instead:

```bash
tether status            # is the daemon up, is mDNS advertising
tether pending           # the requests waiting, with their fingerprints
tether accept 9a4f21c8…
tether devices           # what is paired now
```

If the phone cannot find the machine, mDNS or the firewall is usually why:

```bash
sudo systemctl enable --now avahi-daemon    # tetherd needs it to advertise
sudo ufw allow tether                       # 5134/tcp and 5353/udp
```

The package ships profiles for ufw and firewalld but applies neither. Opening
5134 is your call: it is the mTLS listener, and both ends pin certificates, but
it is still a port.

## Use it

```bash
tether send ./report.pdf             # to the phone
tether status                        # what arrived, and from whom
tether bt threads                    # conversations, over Bluetooth
tether bt send <thread> "on my way"
```

Files the phone sends land in `$XDG_DOWNLOAD_DIR`, or `~/Downloads`. On a server
where that does not exist, set `XDG_DOWNLOAD_DIR` in the unit's environment:

```bash
systemctl --user edit tetherd.service
# [Service]
# Environment=XDG_DOWNLOAD_DIR=%h/inbox
```

## With a process supervisor

Set `TETHER_NO_AUTOSTART=1` for clients when your supervisor owns `tetherd`, so a
status check or CLI command cannot spawn another daemon during an outage.
Set `TETHER_LOG_STDERR=1` for the daemon to keep logs on stderr instead of
redirecting nonterminal output into its state-directory log. Only the exact value
`1` enables either option; defaults for desktop/package installations are unchanged.

An absolute, nonempty `XDG_DOWNLOAD_DIR` overrides the normal GLib/HOME download
lookup. Received files are recorded for status/subscribers independently of desktop
notifications. With no Wayland display, pairing waits for CLI approval; failure to
launch a desktop dialog does not by itself reject or accept the request.

## Clipboard sync

`tether status` says `Clipboard: off, no Wayland session on this machine` and
that is the whole story: without a compositor there is no clipboard to sync. The
rest of the daemon does not care. A compositor that starts later, such as at
login after the unit started at boot, is picked up within a few seconds, with
or without `WAYLAND_DISPLAY`. If the machine does run a compositor and this
still says off, the compositor is missing `wlr-data-control` or
`ext-data-control`.

## From another machine

The CLI talks to a remote daemon over the same TLS the phone uses:

```bash
tether -g --host 10.0.0.5
tether pair --host 10.0.0.5
```
