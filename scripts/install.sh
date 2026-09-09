#!/bin/sh
# Install, update and remove tether without a desktop in the way.
#
#   curl -fsSL https://raw.githubusercontent.com/zackb/tether/main/scripts/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/zackb/tether/main/scripts/install.sh | sh -s -- --method appimage
#   sh scripts/install.sh update
#   sh scripts/install.sh uninstall
#
# It picks the distro package when it can use one, and the AppImage otherwise,
# unpacked into your home directory so it needs neither root nor FUSE. Nothing
# is enabled or started unless you ask for it with --enable.
set -eu

REPO="zackb/tether"
API="https://api.github.com/repos/$REPO/releases"
RAW="https://raw.githubusercontent.com/$REPO/main/scripts/install.sh"

COMMAND="install"
METHOD="auto"
VERSION=""
PREFIX=""
WANT_SERVICE=1
WANT_ENABLE=0
WANT_EXTENSIONS=1
PURGE=0
ASSUME_YES=0

# ---------------------------------------------------------------- output

log() { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
tether installer

Usage:
  install.sh [install|update|uninstall|status|version|help] [options]

Commands:
  install      Install tether (the default).
  update       Replace an install with the current release.
  uninstall    Remove what this installer put on the machine.
  status       Print what is installed and where.

Options:
  --method <auto|deb|rpm|appimage|source>
                   How to install. Default: auto, which prefers the distro
                   package when it can use one and the AppImage otherwise.
  --version <tag>  A release to install, e.g. v0.2.28. Default: the latest.
  --prefix <dir>   Where a portable install goes. Default: ~/.local
  --service        Install the tetherd systemd user service (default).
  --no-service     Do not install it.
  --enable         Enable and start the service, and keep the user session
                   alive after logout, which a server needs.
  --headless       No desktop on this machine: prefer the AppImage, and skip
                   the browser and mail extension hosts.
  --no-extensions  Skip the browser and mail extension hosts.
  --purge          With uninstall: delete pairings, messages and settings too.
  -y, --yes        Do not stop to confirm anything, and let update reinstall
                   the release that is already there.
  -h, --help       This.
EOF
}

# ---------------------------------------------------------------- environment

need() {
    command -v "$1" >/dev/null 2>&1
}

require() {
    need "$1" || die "$1 is needed and not installed."
}

# Downloads to stdout, curl or wget, whichever is here.
fetch() {
    if need curl; then
        curl -fsSL "$1"
    elif need wget; then
        wget -qO- "$1"
    else
        die "neither curl nor wget is installed."
    fi
}

fetch_to() {
    if need curl; then
        curl -fsSL -o "$2" "$1"
    elif need wget; then
        wget -qO "$2" "$1"
    else
        die "neither curl nor wget is installed."
    fi
}

# Runs a command as root, or explains what to run when it cannot.
as_root() {
    if [ "$(id -u)" = "0" ]; then
        "$@"
    elif need sudo; then
        sudo "$@"
    else
        die "this needs root and sudo is not installed. Run: $*"
    fi
}

can_root() {
    [ "$(id -u)" = "0" ] || need sudo
}

detect_arch() {
    case "$(uname -m)" in
        x86_64 | amd64) echo x86_64 ;;
        aarch64 | arm64) echo aarch64 ;;
        *) uname -m ;;
    esac
}

# apt, dnf, pacman, zypper, or none.
detect_pm() {
    if need pacman; then
        echo pacman
    elif need apt-get; then
        echo apt
    elif need dnf; then
        echo dnf
    elif need zypper; then
        echo zypper
    else
        echo none
    fi
}

# ---------------------------------------------------------------- locations

xdg_dir() {
    # $1 is the XDG variable, $2 the fallback relative to $HOME.
    eval "value=\${$1:-}"
    case "$value" in
        /*) printf '%s/tether\n' "$value" ;;
        *) printf '%s/%s/tether\n' "$HOME" "$2" ;;
    esac
}

DATA_DIR=""
CONFIG_DIR=""
STATE_DIR=""
MANIFEST=""

set_dirs() {
    [ -n "${HOME:-}" ] || die "\$HOME is not set."
    DATA_DIR=$(xdg_dir XDG_DATA_HOME .local/share)
    CONFIG_DIR=$(xdg_dir XDG_CONFIG_HOME .config)
    STATE_DIR=$(xdg_dir XDG_STATE_HOME .local/state)
    MANIFEST="$DATA_DIR/install-manifest"
    [ -n "$PREFIX" ] || PREFIX="$HOME/.local"
}

manifest_get() {
    [ -f "$MANIFEST" ] || return 1
    value=$(sed -n "s/^$1=//p" "$MANIFEST" | tail -n 1)
    [ -n "$value" ] || return 1
    printf '%s\n' "$value"
}

# ---------------------------------------------------------------- releases

release_json() {
    if [ -n "$VERSION" ]; then
        fetch "$API/tags/$VERSION"
    else
        fetch "$API/latest"
    fi
}

# The tag and the asset urls of the release we are installing, cached so the
# API is asked once.
RELEASE_TAG=""
RELEASE_ASSETS=""

load_release() {
    [ -z "$RELEASE_TAG" ] || return 0

    json=$(release_json) || die "could not reach the GitHub releases API."
    RELEASE_TAG=$(printf '%s' "$json" | sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p' | head -n 1)
    [ -n "$RELEASE_TAG" ] || die "no release found${VERSION:+ for $VERSION}."

    # One url per line, which is all the asset picking below needs.
    RELEASE_ASSETS=$(printf '%s' "$json" | tr ',' '\n' | sed -n 's/.*"browser_download_url" *: *"\([^"]*\)".*/\1/p')
    [ -n "$RELEASE_ASSETS" ] || die "release $RELEASE_TAG has no downloads."
}

asset_url() {
    printf '%s\n' "$RELEASE_ASSETS" | grep -i -- "$1" | head -n 1
}

# GitHub serves these over TLS and the releases are built in the project's own
# CI. When a release publishes checksums, use them.
verify_checksum() {
    file="$1"
    sums_url=$(asset_url 'SHA256SUMS' || true)
    [ -n "$sums_url" ] || return 0
    need sha256sum || return 0

    sums="$TMP/SHA256SUMS"
    fetch_to "$sums_url" "$sums" || return 0

    want=$(grep -F "$(basename "$file")" "$sums" | awk '{print $1}' | head -n 1)
    [ -n "$want" ] || return 0

    got=$(sha256sum "$file" | awk '{print $1}')
    [ "$want" = "$got" ] || die "checksum mismatch for $(basename "$file")."
    note "checksum verified"
}

# ---------------------------------------------------------------- install

TMP=""

make_tmp() {
    # update calls install, and a second trap would orphan the first directory.
    [ -z "$TMP" ] || return 0
    TMP=$(mktemp -d "${TMPDIR:-/tmp}/tether-install.XXXXXX")
    trap 'rm -rf "$TMP"' EXIT INT TERM
}

choose_method() {
    [ "$METHOD" = "auto" ] || return 0

    arch=$(detect_arch)
    if [ "$arch" != "x86_64" ]; then
        note "no $arch release is published, building from source"
        METHOD="source"
        return 0
    fi

    case "$(detect_pm)" in
        apt) METHOD="deb" ;;
        dnf | zypper) METHOD="rpm" ;;
        pacman)
            # There is an AUR package, but building it is makepkg's job, not this
            # script's, so say so and take the portable route.
            note "on Arch, 'yay -S tether' is the packaged route; installing the AppImage"
            METHOD="appimage"
            ;;
        *) METHOD="appimage" ;;
    esac

    # The packages pull GTK in. A machine with no desktop does not want that.
    if [ "$METHOD" != "appimage" ] && ! can_root; then
        note "no root here, so installing the AppImage into $PREFIX"
        METHOD="appimage"
    fi
}

install_package() {
    load_release
    case "$METHOD" in
        deb) url=$(asset_url '\.deb$') ;;
        rpm) url=$(asset_url '\.rpm$') ;;
    esac
    [ -n "$url" ] || die "release $RELEASE_TAG has no $METHOD package."

    file="$TMP/$(basename "$url")"
    log "downloading $(basename "$url")"
    fetch_to "$url" "$file"
    verify_checksum "$file"

    log "installing it"
    case "$(detect_pm)" in
        apt) as_root apt-get install -y "$file" ;;
        dnf) as_root dnf install -y "$file" ;;
        zypper) as_root zypper --non-interactive install --allow-unsigned-rpm "$file" ;;
        *) die "no package manager to install $METHOD with." ;;
    esac

    BIN_DIR="/usr/bin"
}

install_appimage() {
    load_release
    url=$(asset_url "$(detect_arch).AppImage\$")
    [ -n "$url" ] || url=$(asset_url '\.AppImage$')
    [ -n "$url" ] || die "release $RELEASE_TAG has no AppImage."

    file="$TMP/$(basename "$url")"
    log "downloading $(basename "$url")"
    fetch_to "$url" "$file"
    verify_checksum "$file"
    chmod +x "$file"

    BIN_DIR="$PREFIX/bin"
    APP_DIR="$DATA_DIR/opt"
    mkdir -p "$BIN_DIR" "$APP_DIR"

    # An AppImage needs FUSE to mount itself, which a server usually has no
    # reason to have installed, so unpack it and run the contents directly.
    rm -rf "$APP_DIR/squashfs-root"
    log "unpacking into $APP_DIR"
    (cd "$APP_DIR" && "$file" --appimage-extract >/dev/null) ||
        die "could not unpack the AppImage."

    rm -rf "$APP_DIR/AppDir"
    mv "$APP_DIR/squashfs-root" "$APP_DIR/AppDir"

    write_wrapper "$BIN_DIR/tether" ''
    write_wrapper "$BIN_DIR/tether-gtk" '' gui
    write_wrapper "$BIN_DIR/tetherd" '--daemon'
}

# AppRun routes on its first argument: --daemon is the daemon, no argument at
# all is the GTK app, and anything else is the CLI. So the CLI wrapper asks for
# help when it is given nothing, rather than opening a window.
write_wrapper() {
    target="$1"
    lead="$2"
    kind="${3:-cli}"

    # shellcheck disable=SC2016  # the wrapper's own variables, written literally
    {
        echo '#!/bin/sh'
        echo '# Written by tether install.sh. Edits are lost on the next update.'
        printf 'APPDIR="%s/AppDir"\n' "$APP_DIR"
        if [ "$kind" = "cli" ] && [ -z "$lead" ]; then
            echo '[ $# -gt 0 ] || set -- --help'
        fi
        if [ -n "$lead" ]; then
            printf 'exec "$APPDIR/AppRun" %s "$@"\n' "$lead"
        else
            echo 'exec "$APPDIR/AppRun" "$@"'
        fi
    } >"$target"
    chmod +x "$target"
}

install_source() {
    require git
    require cmake

    src="$DATA_DIR/src"
    log "building from source in $src"
    if [ -d "$src/.git" ]; then
        (cd "$src" && git fetch --quiet origin && git reset --quiet --hard origin/main)
    else
        rm -rf "$src"
        git clone --quiet --depth 1 "https://github.com/$REPO.git" "$src"
    fi

    (cd "$src" && sh scripts/ci-deps.sh) || warn "could not install the build dependencies; carrying on"
    (cd "$src" && make release) || die "the build failed."
    (cd "$src" && as_root cmake --install build/release) || die "the install failed."

    BIN_DIR="/usr/local/bin"
    [ -x "$BIN_DIR/tether" ] || BIN_DIR="/usr/bin"
    RELEASE_TAG="source"
}

# The copy 'tether update' runs. Prefer the one in this checkout, and fall back
# to the network when the script was piped into a shell and has no file of its
# own.
save_installer() {
    self="$0"
    mkdir -p "$DATA_DIR"
    if [ -f "$self" ] && [ "$self" != "sh" ] && [ "$self" != "-" ]; then
        cp "$self" "$DATA_DIR/install.sh"
    else
        fetch_to "$RAW" "$DATA_DIR/install.sh" || return 0
    fi
    chmod +x "$DATA_DIR/install.sh"
}

write_manifest() {
    mkdir -p "$DATA_DIR"
    {
        echo "# Written by tether install.sh. Uninstall reads it."
        echo "method=$METHOD"
        echo "version=$RELEASE_TAG"
        echo "prefix=$PREFIX"
        echo "bindir=$BIN_DIR"
        echo "installed=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >"$MANIFEST"
}

install_service() {
    [ "$WANT_SERVICE" = "1" ] || return 0
    need systemctl || {
        note "no systemd here, so no service was installed"
        return 0
    }

    unit="$CONFIG_DIR/systemd/user/tetherd.service"
    case "$METHOD" in
        deb | rpm | source)
            # The package ships the unit in the system directory already.
            note "the package installed the tetherd user service"
            ;;
        *)
            # A release older than this script has no --install-service. That is
            # not a reason to fail an install that otherwise worked.
            if "$BIN_DIR/tether" --install-service >/dev/null 2>&1; then
                # Point it at the wrapper rather than at whatever the CLI
                # resolved: the wrapper is what survives the next update.
                sed -i "s|^ExecStart=.*|ExecStart=$BIN_DIR/tetherd|" "$unit"
                note "wrote $unit"
            else
                warn "this tether cannot write the user service; skipping it"
                WANT_ENABLE=0
            fi
            ;;
    esac

    if [ "$WANT_ENABLE" = "1" ]; then
        systemctl --user daemon-reload || true
        systemctl --user enable --now tetherd.service ||
            warn "could not enable tetherd.service; is this a systemd user session?"
        if need loginctl; then
            # Without lingering the daemon dies with the SSH session.
            loginctl enable-linger "$(id -un)" >/dev/null 2>&1 ||
                warn "could not enable lingering; the daemon will stop when you log out."
        fi
    fi
}

install_extensions() {
    [ "$WANT_EXTENSIONS" = "1" ] || return 0
    case "$METHOD" in
        deb | rpm | source) return 0 ;; # installed system-wide by the package
    esac
    "$BIN_DIR/tether" --install-extension-host >/dev/null 2>&1 || true
}

check_path() {
    case ":$PATH:" in
        *":$BIN_DIR:"*) return 0 ;;
    esac
    warn "$BIN_DIR is not on your PATH. Add this to your shell profile:"
    # shellcheck disable=SC2016  # $PATH is for the profile to expand, not us
    printf '\n    export PATH="%s:$PATH"\n\n' "$BIN_DIR"
}

do_install() {
    set_dirs
    make_tmp
    choose_method

    case "$METHOD" in
        deb | rpm) install_package ;;
        appimage) install_appimage ;;
        source) install_source ;;
        *) die "unknown method: $METHOD" ;;
    esac

    save_installer
    write_manifest
    install_service
    install_extensions
    check_path

    log "tether $RELEASE_TAG installed"
    printf '\n'
    note "tether status            what the daemon is doing"
    note "tether pending           pairing requests waiting for you"
    note "tether accept <hash>     accept one"
    note "tether --help            everything else"
    if [ "$WANT_SERVICE" = "1" ] && [ "$WANT_ENABLE" != "1" ]; then
        printf '\n'
        note "Start the daemon with:"
        note "  systemctl --user enable --now tetherd.service"
        note "  loginctl enable-linger \$USER      # on a server you log out of"
    fi
}

# ---------------------------------------------------------------- update

do_update() {
    set_dirs
    if installed=$(manifest_get method); then
        METHOD="$installed"
        PREFIX=$(manifest_get prefix || echo "$PREFIX")
        was=$(manifest_get version || echo "unknown")
    else
        was="unknown"
    fi

    make_tmp
    choose_method
    if [ "$METHOD" != "source" ]; then
        load_release
        if [ "$was" = "$RELEASE_TAG" ]; then
            log "tether $was is already the current release"
            [ "$ASSUME_YES" = "1" ] || return 0
        fi
    fi

    log "updating from $was"
    do_install
}

# ---------------------------------------------------------------- uninstall

do_uninstall() {
    set_dirs
    method=$(manifest_get method || echo "$METHOD")
    bindir=$(manifest_get bindir || echo "$PREFIX/bin")

    if need systemctl; then
        systemctl --user disable --now tetherd.service >/dev/null 2>&1 || true
    fi
    rm -f "$CONFIG_DIR/systemd/user/tetherd.service"

    case "$method" in
        deb) as_root apt-get remove -y tether || true ;;
        rpm)
            if need dnf; then as_root dnf remove -y tether || true; else as_root zypper --non-interactive remove tether || true; fi
            ;;
        source)
            src="$DATA_DIR/src"
            [ ! -d "$src/build/release" ] ||
                (cd "$src" && as_root cmake --build build/release --target uninstall >/dev/null 2>&1) || true
            rm -rf "$src"
            ;;
        *)
            rm -f "$bindir/tether" "$bindir/tether-gtk" "$bindir/tetherd"
            rm -rf "$DATA_DIR/opt"
            ;;
    esac

    # Native messaging manifests this user's install wrote.
    for dir in "$HOME/.mozilla/native-messaging-hosts" \
        "$HOME/.thunderbird/native-messaging-hosts" \
        "$HOME/.betterbird/native-messaging-hosts" \
        "$HOME/.config/chromium/NativeMessagingHosts" \
        "$HOME/.config/google-chrome/NativeMessagingHosts"; do
        rm -f "$dir/com.tether.extension.json"
    done

    rm -f "$MANIFEST" "$DATA_DIR/install.sh"

    if [ "$PURGE" = "1" ]; then
        log "removing pairings, messages and settings"
        rm -rf "$DATA_DIR" "$CONFIG_DIR" "$STATE_DIR"
    else
        note "pairings and settings are still in $CONFIG_DIR (--purge removes them)"
    fi

    log "tether removed"
}

# ---------------------------------------------------------------- status

do_status() {
    set_dirs
    if [ ! -f "$MANIFEST" ]; then
        log "this installer has not installed tether on this machine"
        need tether && note "there is a tether on PATH at $(command -v tether)"
        return 0
    fi

    log "installed by this installer"
    sed -n 's/^\([a-z]*\)=/  \1: /p' "$MANIFEST"
    if need systemctl; then
        state=$(systemctl --user is-active tetherd.service 2>/dev/null || true)
        note "service: ${state:-not installed}"
    fi
}

# ---------------------------------------------------------------- arguments

while [ $# -gt 0 ]; do
    case "$1" in
        install | update | upgrade | uninstall | remove | status | version | help)
            case "$1" in
                upgrade) COMMAND="update" ;;
                remove) COMMAND="uninstall" ;;
                *) COMMAND="$1" ;;
            esac
            ;;
        --method)
            [ $# -ge 2 ] || die "--method needs a value."
            METHOD="$2"
            shift
            ;;
        --method=*) METHOD="${1#--method=}" ;;
        --version)
            [ $# -ge 2 ] || die "--version needs a value."
            VERSION="$2"
            shift
            ;;
        --version=*) VERSION="${1#--version=}" ;;
        --prefix)
            [ $# -ge 2 ] || die "--prefix needs a value."
            PREFIX="$2"
            shift
            ;;
        --prefix=*) PREFIX="${1#--prefix=}" ;;
        --service) WANT_SERVICE=1 ;;
        --no-service) WANT_SERVICE=0 ;;
        --enable) WANT_ENABLE=1 ;;
        --headless)
            [ "$METHOD" != "auto" ] || METHOD="appimage"
            WANT_EXTENSIONS=0
            ;;
        --no-extensions) WANT_EXTENSIONS=0 ;;
        --purge) PURGE=1 ;;
        -y | --yes) ASSUME_YES=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *) die "unknown argument: $1. Run with --help." ;;
    esac
    shift
done

case "$METHOD" in
    auto | deb | rpm | appimage | source) ;;
    *) die "unknown method: $METHOD" ;;
esac

[ "$(uname -s)" = "Linux" ] || die "tether runs on Linux."

case "$COMMAND" in
    install) do_install ;;
    update) do_update ;;
    uninstall) do_uninstall ;;
    status) do_status ;;
    version)
        set_dirs
        manifest_get version || echo "not installed by this installer"
        ;;
    help) usage ;;
esac
