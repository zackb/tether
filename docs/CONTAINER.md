# Tether in a container

The container runs `tetherd`, a private session D-Bus, and `obexd`. Drive it with
`docker compose exec tether tether ...`; there is **no web UI in this image**.

This is a local-build option, not a published registry image. Automated tests
cover the image and headless behavior; a real iPhone/Bluetooth validation is
still required for a particular host. See the acceptance checklist below.

## What runs where

| Host Linux machine | Container |
|---|---|
| Bluetooth adapter and BlueZ (`bluetoothd`), including Bluetooth bonds | Non-root `tetherd` and CLI |
| Avahi/mDNS, firewall, system D-Bus | Private session D-Bus and foreground OBEX daemon |
| Persistent directories owned by your chosen user | Keys/config/history in `/data`, received files in `/downloads` |

Wi-Fi pairing and file transfer use the Tether iPhone app. **SMS/iMessage needs
the iPhone within Bluetooth range of this Linux machine.** A remote browser or
container cannot turn Bluetooth messaging into an internet transport.

The first supported deployment shape is ordinary Linux Docker with host
networking on amd64 or arm64. Rootless/user-namespace-remapped Docker,
Kubernetes, Docker Desktop Bluetooth passthrough, calls/audio, AirPods and
notification mirroring are not validated by this deployment. Clipboard sync
needs a Wayland session and is not available here.

## Host prerequisites

1. Configure BlueZ and the adapter using [the Bluetooth instructions](BLUETOOTH.md)
   and [README prerequisites](../README.md#bluetooth-for-messages-and-notifications).
   In particular, configure the experimental bearer API and adapter class **on
   the host before pairing**, then enable the iPhone's Messages/Contacts permissions.
   Upstream currently recommends BlueZ 5.86+ on the host. The Ubuntu 26.04 image
   uses its own distro-packaged OBEX client, which includes the upstream RX/TX MTU
   correction. Updating the image does not update the host's `bluetoothd`.
   Record both versions when testing.
2. Run Avahi and the system bus on the host. Open inbound 5134/tcp and mDNS
   5353/udp on the trusted local network; see the README's firewall instructions.
3. Verify the host settings, then set `TETHER_BLUEZ_SECURE_CONNECTIONS=1` in
   the container environment. Tether can detect BlueZ's experimental API over
   D-Bus, but the container cannot run host management tools to inspect the
   controller's Secure Connections setting.
4. Choose a real non-root host UID/GID authorized to use the host system bus and
   Bluetooth services. Do not run another `tetherd` on that host port/adapter.

The example mounts `/run/dbus` read-only at `/host/run/dbus`. **This is privileged
access to host services: a read-only mount does not make D-Bus calls read-only.**
Host D-Bus policy checks the configured identity. Some distributions require a
supplemental host group: add only that numeric GID to Compose's `group_add` if
needed. Do not use `privileged: true`, wildcard D-Bus policy grants, host PID
sharing or broad device mounts to work around an unexplained permission error.

Bluetooth bonds remain in the host's BlueZ storage, not the Tether data volume.
The container never installs host service files, changes the firewall, kills a
host daemon, or automatically confirms pairing requests.

## Build and start

From the repository root, as the non-root user that will own the data:

```sh
# Choose paths outside the repository/build context.
mkdir -p "$HOME/.local/share/tether-container" "$HOME/tether-inbox"
chmod 700 "$HOME/.local/share/tether-container"

cp packaging/container/.env.example packaging/container/.env
id -u
id -g
```

Edit `packaging/container/.env`:

- Set `TETHER_UID`/`TETHER_GID` to the values above (not root).
- Set `TETHER_DATA_DIR`/`TETHER_DOWNLOADS_DIR` to the **absolute**, pre-created host
  paths. Replace the example `/home/your-user` values; do not use literal `$HOME`.
- Optionally choose `TETHER_HOSTNAME`, a DNS-style host label such as `tether-pi`.
  It is the advertised display name; the persisted certificate is the identity.
- Leave `TETHER_BLUEZ_SECURE_CONNECTIONS=1` only after checking that the host
  controller meets that prerequisite. Set it to `0` when the capability is absent.

The entrypoint refuses wrong ownership or a non-private data directory rather
than recursively changing your files. Existing directories must already be
owned by the selected UID. Missing source directories are not auto-created.

```sh
docker compose --env-file packaging/container/.env \
  -f packaging/container/compose.yaml config

docker compose --env-file packaging/container/.env \
  -f packaging/container/compose.yaml up -d --build
```

For the remaining examples, work in `packaging/container` so Compose finds both
`compose.yaml` and `.env`:

```sh
cd packaging/container
docker compose logs -f --tail 100
docker compose exec tether tether status
```

The image is `tether:local`, built for your machine's architecture. Build a
specific architecture with Docker's `--platform linux/amd64` or
`--platform linux/arm64`; executing cross-architecture images requires emulation.
No build or workflow pushes an image to a registry.

The reference deployment uses host networking, so Docker port mappings do not
apply: `tetherd` listens on host port 5134. Limit access with the host firewall.
Only the existing mTLS phone/peer endpoint listens on TCP; the local control
socket is **not** exposed on the network.

## Pair and use the CLI

### Wi-Fi

The container shows Tether on every network the host is on, and the host
firewall decides who reaches it. For a host that moves between networks, set
`TETHER_LAN_SCOPE=trusted` in `.env`, then trust each network once (trust is
kept in `/data`):

```sh
docker compose exec tether tether --networks
docker compose exec tether tether --trust-network <key>
```

Open the Tether iPhone app on the same discoverable network, select this host,
and compare the fingerprint before approving:

```sh
docker compose exec tether tether pending
docker compose exec tether tether accept <fingerprint>
docker compose exec tether tether status
```

Without Wayland, no desktop approval window is launched. A missing/broken GUI
helper is not a rejection: the request waits for explicit approval through the
CLI. Neither the container nor its health probe accepts devices automatically.

### Bluetooth messages

```sh
docker compose exec tether tether bt devices
docker compose exec tether tether bt pair AA:BB:CC:DD:EE:FF
docker compose exec tether tether bt connection
docker compose exec tether tether bt threads
docker compose exec tether tether bt messages 'tel:+15555550100'
# Sends a real message. Choose your own recipient and content.
docker compose exec tether tether bt send 'tel:+15555550100' 'test message'
```

Keep the interactive terminal for pairing and confirm the matching passkey on
both ends. Enable **Show Message Notifications** and **Sync Contacts** in the
iPhone's Bluetooth settings for this computer. Some permission toggles take time
to appear. These operations use the host's adapter; don't delete existing bonds
unless you intend to re-pair that phone.

If a send times out/disconnects, check the phone before retrying. The container
does not queue or replay messages, and a lost response does not prove the phone
failed to send.

### Files

Send a file from the iPhone share sheet/app; it lands in your configured inbox:

```sh
docker compose exec tether tether status
# Place a file into your host inbox first; the daemon sees the container path.
docker compose exec tether tether send /downloads/report.pdf
```

Files remain after a container restart. The recent-received list in `status` is
memory-only and resets on restart; it is not a durable file catalog. Successful
receives are recorded even without a desktop notification service. Incomplete
transfers are not reported as successful; no resumable-transfer guarantee is added.

## Persistence and secrets

| Container path | Contents |
|---|---|
| `/data/.config/tether` | TLS certificate/private key, trusted peers, Bluetooth configuration, headless history key |
| `/data/.local/share/tether` | Message journal and contact cache |
| `/data/.local/state/tether` | Application state (normal container logs go to stderr instead) |
| `/downloads` | Received files |
| `/run/tether-runtime` | Private tmpfs: session bus, `tether/tetherd.sock`, caches and OBEX staging |

XDG roots equal their HOME-based defaults, avoiding accidental migration to a
second config tree. The image declares these variables so `docker exec` and the
supervised daemon agree. Configure mounts instead of overriding these paths.

Encrypted history remains the default. With no desktop secret service, Tether
keeps its history key in a mode-0600 `store.key` under the config directory.
**The key and encrypted data are on the same volume.** Encryption does not
protect against theft of that entire volume or backup. Keep `/data` and its
backups private and restore the key and journal together; don't downgrade to
plaintext to work around missing keys.

To take a consistent backup, stop the container and copy both persistent host
directories, preserving ownership/permissions. Do not back up or share the
runtime tmpfs or a desktop user's session bus. Recreating the container with the
same volumes preserves Wi-Fi identity/trust. Deleting `/data` creates a new
identity and requires pairing again. Migrating to another Bluetooth host also
requires handling that host's bonds separately.

## Lifecycle, health and diagnostics

- All application processes run as the selected UID with capabilities dropped
  and a read-only root filesystem. The image uses `tini` and a supervisor script,
  not systemd.
- The supervisor checks the host bus, starts its session bus and OBEX with bounded
  readiness waits, then starts `tetherd`. If any managed child exits, its siblings
  are terminated and the container exits nonzero.
- The image makes root-owned `/run/systemd` inaccessible to the non-root OBEX
  process (mode `0700`). Without a logind session, this selects OBEX's built-in
  login-monitor failure fallback, which treats the user as active. It does not
  grant access to host login sessions or require running OBEX as root.
- SIGTERM stops the children within the 15-second Compose grace period. Docker
  manages restart/backoff (`unless-stopped`); permanent setup errors remain
  visible in logs rather than gaining permissions automatically.
- Health checks ask the existing daemon for status and check session-bus/OBEX
  presence. They never spawn `tetherd`, activate OBEX, send messages or pair a
  device. A missing phone, powered-off adapter or missing mDNS does not by itself
  fail liveness. A hung daemon/bus causes the probe to fail within five seconds.
- Docker does **not** restart a still-running container just because it is
  unhealthy. Inspect logs and restart deliberately if needed.

These environment switches are also usable outside Docker:

| Variable | Value | Behavior |
|---|---|---|
| `TETHER_LOG_STDERR` | `1` | Keep daemon stderr attached to its supervisor |
| `TETHER_NO_AUTOSTART` | `1` | Prevent CLI/client helpers from spawning `tetherd` |
| `TETHER_BLUEZ_SECURE_CONNECTIONS` | Boolean (`1`/`0`, `true`/`false`, `yes`/`no`, `on`/`off`) | Declare whether the host controller has Secure Connections enabled |

Unset runtime switches preserve normal behavior. The local `btmgmt` result takes
precedence when available; the capability declaration is used only when that probe
cannot inspect the host controller from inside a container.

`XDG_DOWNLOAD_DIR` now takes precedence when nonempty and absolute; invalid or
empty overrides fall back to the existing GLib/HOME lookup.

Troubleshooting:

- **`wrong-owner` / `invalid-layout`:** correct the host directory ownership,
  private `/data` permissions, UID/GID or required environment. Do not run as root.
- **`host-bus-missing` / `host-bus-denied`:** check `/run/dbus`, the host identity
  and host D-Bus policy. Capture the exact failure before considering any access change.
- **OBEX cannot start/connect:** record the image/host BlueZ versions and bus
  errors. Non-root Bluetooth/OBEX operation is a hardware acceptance gate, not a
  reason to enable privileged mode silently.
- **Phone not discoverable:** confirm Avahi, host firewall and local multicast
  connectivity. This container cannot make mDNS cross an isolated network/VPN.
- **Misleading `tether bt setup` advice:** the experimental-API check reads
  BlueZ's own objects now, so the container's PID namespace no longer hides the
  answer. Set `TETHER_BLUEZ_SECURE_CONNECTIONS` if the `btmgmt` probe lacks
  permission; don't share host PID space or add management capabilities just to
  improve a diagnostic.
- **Host service repaired/restarted:** Tether may need a container restart if
  BlueZ was unavailable during startup. This packaging does not promise new
  D-Bus reconnection behavior.
- **Port already in use:** stop the competing Tether instance intentionally.
  The container will not kill or replace it.

Compose rotates Docker logs (10 MB × 3 files). Existing Tether logs can contain
device identifiers, recipient addresses and file paths: treat them as sensitive,
not as an automatically redacted support bundle.

## Automated tests

From the repository root:

```sh
docker build --target test -f packaging/container/Dockerfile -t tether-test:local .
docker build --target runtime -f packaging/container/Dockerfile -t tether:local .
scripts/test-container.sh tether:local tether-test:local
```

The test stage runs GTest/CTest as non-root. The smoke script creates disposable
volumes and a private stand-in system bus, never mounts host services or touches
a real adapter. It tests supervised child failures, bounded health, restart
persistence including a nonempty encrypted journal, volume ownership and
permission constraints. Its root initialization command only prepares its own
throwaway volumes; the tested runtime remains non-root. Traps clean up test
containers and volumes on exit.

`.github/workflows/container.yml` runs this on native amd64 and arm64 Linux
runners, without registry credentials or publishing. Discovery tests that need
Avahi may explicitly skip when that service is absent. A passing build or
synthetic socket/bus test is not proof of Bluetooth/iPhone compatibility.

## Real-iPhone acceptance checklist

Before calling a host deployment functional:

- [ ] Record architecture, distro/kernel, Docker/image identity, BlueZ/OBEX,
      adapter and iOS/Tether app versions. Redact personal identifiers when sharing.
- [ ] With fresh private state and no desktop, discover/pair over Wi-Fi, compare
      fingerprints and explicitly accept; both ends report paired.
- [ ] Pair Bluetooth through the CLI, confirm both passkeys and iPhone permissions.
- [ ] Read a one-to-one thread, send an operator-approved test message to a
      consenting recipient, verify it on the phone/recipient and receive a reply.
- [ ] Transfer a benign file in each direction; compare bytes/checksum and see the
      receive status without a desktop notification service.
- [ ] Recreate the container with the same volumes: retain fingerprint/trust,
      nonempty message history and downloaded files; reconnect without fresh pairing.
- [ ] Take the phone out of range and restore it: daemon stays alive and existing
      transport reconnection works; no competing daemon is spawned.
- [ ] Stop/restart cleanly without orphaned processes or host-service takeover.

Run this on at least one Bluetooth-equipped Linux architecture after automated
checks pass on both. Explicitly label any other architecture as hardware-unverified.
The web gateway/UI remains a separate follow-up to issue #147.
