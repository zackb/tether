#!/bin/bash

# Tether Release Automation Script
# Usage: ./scripts/release.sh <version>
# Example: ./scripts/release.sh 0.1.2

set -e

VERSION=$1

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> (e.g., 0.1.2)"
    exit 1
fi

TAG="v$VERSION"
REPO_ROOT=$(git rev-parse --show-toplevel)
AUR_GIT_DIR="$REPO_ROOT/../tether-git"
AUR_BIN_DIR="$REPO_ROOT/../tether-bin"
DEPENDS=$(grep '^depends=' "$REPO_ROOT/PKGBUILD")

# 1. Validation
if ! command -v gh &> /dev/null; then
    echo "Error: 'gh' (GitHub CLI) is not installed."
    exit 1
fi

if ! git diff-index --quiet HEAD --; then
    echo "Error: You have uncommitted changes. Please commit or stash them first."
    exit 1
fi

CURRENT_BRANCH=$(git branch --show-current)
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo "Error: You are on branch '$CURRENT_BRANCH'. Releases must be performed from 'main'."
    exit 1
fi

echo "🚀 Starting release process for $TAG..."

# 2. Update CMakeLists.txt version (optional but recommended)
sed -i "s/project(tether VERSION [0-9.]*/project(tether VERSION $VERSION/" "$REPO_ROOT/CMakeLists.txt"
# Software centers and Flathub show the newest <release> from the metainfo
METAINFO="$REPO_ROOT/src/gtk/io.github.zackb.tether.metainfo.xml"
if ! grep -q "<release version=\"$VERSION\"" "$METAINFO"; then
    sed -i "s|<releases>|<releases>\n    <release version=\"$VERSION\" date=\"$(date +%F)\"/>|" "$METAINFO"
fi
git add "$REPO_ROOT/CMakeLists.txt" "$METAINFO"
git commit -m "chore: bump version to $VERSION" || true

# 3. Tag and Push
echo "🏷️  Tagging $TAG..."
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "Warning: Tag $TAG already exists locally."
else
    git tag -a "$TAG" -m "Release $TAG"
fi
git push origin main
git push origin "$TAG"

# 4. Wait for CI to build and publish the packages
# Packages are built by .github/workflows/release.yml, each in the distro it targets:
# a .deb linked against Arch's glibc and libstdc++ will not start on Debian or Ubuntu.
echo "📦 Waiting for the release workflow..."
RUN_ID=""
for _ in $(seq 30); do
    RUN_ID=$(gh run list --workflow=release.yml --branch "$TAG" --limit 1 --json databaseId --jq '.[0].databaseId')
    [ -n "$RUN_ID" ] && break
    sleep 5
done
if [ -z "$RUN_ID" ]; then
    echo "Error: no release workflow run found for $TAG."
    exit 1
fi
gh run watch "$RUN_ID" --exit-status

# 5. Fetch the published tarball, which tether-bin needs the checksum of
ASSET_DIR="$REPO_ROOT/build/release"
mkdir -p "$ASSET_DIR"
TARBALL="$ASSET_DIR/tether-$VERSION.tar.gz"
rm -f "$TARBALL"
gh release download "$TAG" --pattern "tether-$VERSION.tar.gz" --dir "$ASSET_DIR"

# 6. Update AUR (tether-git)
if [ -d "$AUR_GIT_DIR" ]; then
    echo "🧬 Updating tether-git AUR..."
    sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$AUR_GIT_DIR/PKGBUILD"
    sed -i "s/^pkgrel=.*/pkgrel=1/" "$AUR_GIT_DIR/PKGBUILD"
    sed -i "s/^depends=.*/$DEPENDS/" "$AUR_GIT_DIR/PKGBUILD"
    cp "$REPO_ROOT/packaging/tether.install" "$AUR_GIT_DIR/tether.install"
    (
        cd "$AUR_GIT_DIR"
        makepkg --printsrcinfo > .SRCINFO
        git add PKGBUILD .SRCINFO tether.install
        git commit -m "update to $VERSION"
        git push
    )
    echo "   tether-git updated and pushed."
else
    echo "⚠️  Warning: $AUR_GIT_DIR not found, skipping."
fi

# 7. Update AUR (tether-bin)
if [ -d "$AUR_BIN_DIR" ]; then
    echo "🏗️  Updating tether-bin AUR..."
    SHA256=$(sha256sum "$TARBALL" | cut -d' ' -f1)
    sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$AUR_BIN_DIR/PKGBUILD"
    sed -i "s/^pkgrel=.*/pkgrel=1/" "$AUR_BIN_DIR/PKGBUILD"
    sed -i "s/^depends=.*/$DEPENDS/" "$AUR_BIN_DIR/PKGBUILD"
    sed -i "s/^sha256sums=.*/sha256sums=('$SHA256')/" "$AUR_BIN_DIR/PKGBUILD"
    cp "$REPO_ROOT/packaging/tether.install" "$AUR_BIN_DIR/tether.install"
    (
        cd "$AUR_BIN_DIR"
        makepkg --printsrcinfo > .SRCINFO
        git add PKGBUILD .SRCINFO tether.install
        git commit -m "update to $VERSION"
        git push
    )
    echo "   tether-bin updated and pushed."
else
    echo "⚠️  Warning: $AUR_BIN_DIR not found, skipping."
fi

# 8. Flathub: its bot reads x-checker-data and opens the update PR on flathub/io.github.zackb.tether
echo "📦 Flathub: commit for $TAG is $(git rev-parse "$TAG^{commit}")"

echo "✅ Full release $VERSION successfully deployed to GitHub and AUR!"
