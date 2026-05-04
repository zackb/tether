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
git add "$REPO_ROOT/CMakeLists.txt"
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

# 4. Build Packages
echo "📦 Building packages..."
rm -rf "$REPO_ROOT/build/release"
make package

# 5. Create GitHub Release
echo "🌐 Creating GitHub Release..."
ASSET_DIR="$REPO_ROOT/build/release"
# Find the specific tarball, deb, and rpm
TARBALL="$ASSET_DIR/tether-$VERSION.tar.gz"
DEB=$(ls "$ASSET_DIR"/tether-*.deb | head -n 1)
RPM=$(ls "$ASSET_DIR"/tether-*.rpm | head -n 1)

gh release create "$TAG" "$TARBALL" "$DEB" "$RPM" --title "Release $TAG" --generate-notes

# 6. Update AUR (tether-git)
if [ -d "$AUR_GIT_DIR" ]; then
    echo "🧬 Updating tether-git AUR..."
    sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$AUR_GIT_DIR/PKGBUILD"
    (
        cd "$AUR_GIT_DIR"
        makepkg --printsrcinfo > .SRCINFO
        git add PKGBUILD .SRCINFO
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
    sed -i "s/^sha256sums=.*/sha256sums=('$SHA256')/" "$AUR_BIN_DIR/PKGBUILD"
    (
        cd "$AUR_BIN_DIR"
        makepkg --printsrcinfo > .SRCINFO
        git add PKGBUILD .SRCINFO
        git commit -m "update to $VERSION"
        git push
    )
    echo "   tether-bin updated and pushed."
else
    echo "⚠️  Warning: $AUR_BIN_DIR not found, skipping."
fi

echo "✅ Full release $VERSION successfully deployed to GitHub and AUR!"
