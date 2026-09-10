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
tether --install-service     # writes ~/.config/systemd/user/tetherd.service
```

It never enables or starts anything: it prints the commands above and leaves the
decision to you. `tether --uninstall-service` takes the unit back.

Note the order. A client starts `tetherd` on its own when one is not already
running, so if you have used the CLI at all there is probably a daemon up
already, holding the port this unit wants. Enabling the unit takes it over.
Once the unit is enabled, clients stop spawning their own and leave the daemon
to systemd.

## Pair the iPhone

The daemon advertises itself over mDNS and the iOS app finds it. Both ends have
to accept, and on the desktop that prompt is a GTK window you do not have here,
so accept it from the shell instead:

```bash
tether --status         # is the daemon up, is mDNS advertising
tether --pending        # the requests waiting, with their fingerprints
tether --accept 9a4f21c8…
tether --list-devices   # what is paired now
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
tether -f ./report.pdf                # to the phone
tether --status                       # what arrived, and from whom
tether --bt-threads                   # conversations, over Bluetooth
tether --bt-send <thread> "on my way"
```

Files the phone sends land in `$XDG_DOWNLOAD_DIR`, or `~/Downloads`. On a server
where that does not exist, set `XDG_DOWNLOAD_DIR` in the unit's environment:

```bash
systemctl --user edit tetherd.service
# [Service]
# Environment=XDG_DOWNLOAD_DIR=%h/inbox
```

## Clipboard sync

`tether --status` says `Clipboard: off, no Wayland session on this machine` and
that is the whole story: without a compositor there is no clipboard to sync. The
rest of the daemon does not care. If the machine does run a compositor and this
still says off, the compositor is missing `wlr-data-control` or
`ext-data-control`.

## From another machine

The CLI talks to a remote daemon over the same TLS the phone uses:

```bash
tether -g --host 10.0.0.5
tether --pair --host 10.0.0.5
```
