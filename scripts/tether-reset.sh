#!/usr/bin/env bash
# Delete Tether's pairing state so the next pair starts from nothing.
#
#   tether-reset.sh wifi   mTLS keys, known hosts, pending pair requests
#   tether-reset.sh bt     the BlueZ bond, Bluetooth config, contacts, messages,
#                          and the key the encrypted store was written with
#   tether-reset.sh all    both, plus the daemon log
#
# Paths follow the same XDG variables the daemon resolves, so a custom
# XDG_CONFIG_HOME or XDG_DATA_HOME is cleaned rather than skipped.

set -uo pipefail

scope=${1:-}
assume_yes=0
dry_run=0
for arg in "${@:2}"; do
    case $arg in
        -y|--yes) assume_yes=1 ;;
        -n|--dry-run) dry_run=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

case $scope in
    wifi|bt|all) ;;
    *) echo "usage: $0 {wifi|bt|all} [-y] [--dry-run]" >&2; exit 2 ;;
esac

red=$'\e[31m'; grn=$'\e[32m'; ylw=$'\e[33m'; dim=$'\e[2m'; rst=$'\e[0m'
[[ -t 1 ]] || { red=; grn=; ylw=; dim=; rst=; }

config="${XDG_CONFIG_HOME:-$HOME/.config}/tether"
share="${XDG_DATA_HOME:-$HOME/.local/share}/tether"
state="${XDG_STATE_HOME:-$HOME/.local/state}/tether"
runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/tether"

wifi_paths=(
    "$config/cert.pem"
    "$config/key.pem"
    "$config/known_hosts.json"
    "$runtime/pending_pairs.json"
)
bt_paths=(
    "$config/bluetooth.json"
    "$config/groups.json"
    "$config/store.key"
    "$share/contacts.json"
    "$share/contacts.json.enc"
    "$share/messages.ndjson"
    "$share/messages.ndjson.enc"
)

paths=()
[[ $scope == wifi || $scope == all ]] && paths+=("${wifi_paths[@]}")
[[ $scope == bt   || $scope == all ]] && paths+=("${bt_paths[@]}")
[[ $scope == all ]] && paths+=("$state/tetherd.log")

# The bond lives in BlueZ, not in any of the files above. Read the address before
# bluetooth.json is deleted.
bond_address=""
if [[ $scope == bt || $scope == all ]] && [[ -f $config/bluetooth.json ]]; then
    bond_address=$(sed -n 's/.*"device_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$config/bluetooth.json" | head -n 1)
fi

echo "Resetting: $scope"
echo
for p in "${paths[@]}"; do
    if [[ -e $p ]]; then
        printf '  %sremove%s  %s\n' "$ylw" "$rst" "$p"
    else
        printf '  %sabsent%s  %s\n' "$dim" "$rst" "$p"
    fi
done
[[ -n $bond_address ]] && printf '  %sremove%s  BlueZ bond %s\n' "$ylw" "$rst" "$bond_address"
pgrep -x tetherd >/dev/null && printf '  %sstop%s    tetherd\n' "$ylw" "$rst"
echo

if (( dry_run )); then
    echo "${dim}dry run: nothing was deleted${rst}"
    exit 0
fi

if (( ! assume_yes )); then
    read -r -p "Delete these? [y/N] " reply
    [[ $reply == [yY] ]] || { echo "aborted"; exit 1; }
    echo
fi

# The daemon rewrites known_hosts.json, bluetooth.json and the journal as it runs,
# so it has to be gone before anything is deleted or the reset partially undoes
# itself.
if pgrep -x tetherd >/dev/null; then
    pkill -x tetherd
    for _ in $(seq 20); do
        pgrep -x tetherd >/dev/null || break
        sleep 0.25
    done
    pgrep -x tetherd >/dev/null && pkill -9 -x tetherd
    printf '  %s✓%s stopped tetherd\n' "$grn" "$rst"
fi

if [[ -n $bond_address ]]; then
    if bluetoothctl remove "$bond_address" >/dev/null 2>&1; then
        printf '  %s✓%s removed BlueZ bond %s\n' "$grn" "$rst" "$bond_address"
    else
        printf '  %s!%s could not remove BlueZ bond %s (already gone?)\n' "$ylw" "$rst" "$bond_address"
    fi
fi

for p in "${paths[@]}"; do
    [[ -e $p ]] || continue
    if rm -f "$p"; then
        printf '  %s✓%s %s\n' "$grn" "$rst" "$p"
    else
        printf '  %s✗%s %s\n' "$red" "$rst" "$p"
    fi
done

# The store key normally lives in the desktop keyring, not in a file, so the
# loop above misses it on any machine with a working secret service.
if [[ $scope == bt || $scope == all ]] && command -v secret-tool >/dev/null 2>&1; then
    if secret-tool clear application tether >/dev/null 2>&1; then
        printf '  %s✓%s store key in the desktop keyring\n' "$grn" "$rst"
    fi
fi

echo
echo "Done. The iPhone side cannot be cleared from here:"
if [[ $scope == bt || $scope == all ]]; then
    echo "  - Settings > Bluetooth > (i) next to this computer > Forget This Device"
fi
if [[ $scope == wifi || $scope == all ]]; then
    echo "  - Tether iOS app: remove this computer from its paired devices"
fi
