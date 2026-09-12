#!/bin/bash
# Build a single-file Flatpak bundle of tether.
# Needs flatpak-builder or org.flatpak.Builder,
# and the org.gnome.Platform//49 runtime and matching SDK.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MANIFEST=$ROOT/packaging/flatpak/com.tether.desktop.yml
BUILD_DIR=${BUILD_DIR:-$ROOT/build/flatpak}
APP_ID=com.tether.desktop
RUNTIME_VERSION=49

flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install -y --user --noninteractive flathub \
    "org.gnome.Platform//$RUNTIME_VERSION" "org.gnome.Sdk//$RUNTIME_VERSION"

# flatpak install exits 0 even when it resolved nothing, so check rather than trust.
for ref in org.gnome.Platform org.gnome.Sdk; do
    flatpak info "$ref//$RUNTIME_VERSION" >/dev/null 2>&1 ||
        { echo "$ref//$RUNTIME_VERSION is not installed" >&2; exit 1; }
done

# The distro package if there is one, otherwise the builder ships as a flatpak.
if command -v flatpak-builder >/dev/null; then
    BUILDER=(flatpak-builder)
else
    flatpak install -y --user --noninteractive flathub org.flatpak.Builder
    flatpak info org.flatpak.Builder >/dev/null 2>&1 ||
        { echo "org.flatpak.Builder is not installed" >&2; exit 1; }
    BUILDER=(flatpak run org.flatpak.Builder)
fi

mkdir -p "$BUILD_DIR"
"${BUILDER[@]}" --force-clean --user --install-deps-from=flathub \
    --repo="$BUILD_DIR/repo" \
    "$BUILD_DIR/build" "$MANIFEST"

VERSION=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
VERSION=${VERSION:-unknown}
OUTPUT=$BUILD_DIR/tether-$VERSION-$(uname -m).flatpak

flatpak build-bundle "$BUILD_DIR/repo" "$OUTPUT" "$APP_ID" --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo

echo "==> $OUTPUT"
