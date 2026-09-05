#!/bin/bash
# Install everything needed to build and test tether, on Arch, Fedora, Debian or Ubuntu.
#
#   ./scripts/ci-deps.sh
#
# This installs system-wide.

set -eu

. /etc/os-release

case ${ID_LIKE:-$ID} in
    *arch*)
        pacman -Syu --noconfirm --needed \
            base-devel cmake ninja git pkgconf \
            wayland avahi openssl glib2 libsecret \
            gtk3 gtk-layer-shell libnotify \
            npm zip gettext
        ;;
    *fedora*)
        dnf install -y \
            gcc-c++ cmake ninja-build git pkgconf-pkg-config \
            wayland-devel avahi-devel openssl-devel glib2-devel libsecret-devel \
            gtk3-devel gtk-layer-shell-devel libnotify-devel \
            npm zip rpm-build gettext
        ;;
    *debian*|*ubuntu*)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends \
            g++ cmake ninja-build git pkg-config ca-certificates \
            libwayland-dev libavahi-client-dev libssl-dev libglib2.0-dev libsecret-1-dev \
            libgtk-3-dev libgtk-layer-shell-dev libnotify-dev \
            npm zip gettext
        ;;
    *)
        echo "ci-deps: unsupported distro: ${ID:-unknown}" >&2
        exit 1
        ;;
esac

command -v msgfmt >/dev/null || { echo "ci-deps: msgfmt missing; translations would be skipped" >&2; exit 1; }

pkg-config --print-errors --exists \
    "wayland-client avahi-client openssl glib-2.0 gio-2.0 gio-unix-2.0 libsecret-1 gtk+-3.0 gtk-layer-shell-0 libnotify"
echo "==> ready: gtk $(pkg-config --modversion gtk+-3.0), openssl $(pkg-config --modversion openssl)"
