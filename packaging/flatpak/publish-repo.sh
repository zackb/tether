#!/bin/bash
# Merge per-arch flatpak-builder repos into one signed repo and lay out a static site.
# Usage: FLATPAK_GPG_KEY=<armored secret key> publish-repo.sh BASE_URL SITE_DIR REPO...

set -euo pipefail

[ $# -ge 3 ] || { echo "usage: $0 BASE_URL SITE_DIR REPO..." >&2; exit 2; }
[ -n "${FLATPAK_GPG_KEY:-}" ] || { echo "FLATPAK_GPG_KEY is not set" >&2; exit 2; }

BASE_URL=${1%/}
SITE=$2
shift 2
APP_ID=io.github.zackb.tether
BRANCH=master
RUNTIME_REPO=https://flathub.org/repo/flathub.flatpakrepo
HOMEPAGE=https://github.com/zackb/tether

WORK=$(mktemp -d)
export GNUPGHOME=$WORK/gnupg
trap 'gpgconf --kill gpg-agent 2>/dev/null || true; rm -rf "$WORK"' EXIT
mkdir -m 700 "$GNUPGHOME"

printenv FLATPAK_GPG_KEY | gpg --batch --quiet --import
KEY=$(gpg --list-secret-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }')
PUBLIC_KEY=$(gpg --export "$KEY" | base64 -w0)
SIGN=(--gpg-sign="$KEY" --gpg-homedir="$GNUPGHOME")

DEST=$WORK/repo
ostree --repo="$DEST" init --mode=archive-z2

for src in "$@"; do
    # Debug refs stay out to keep the published repo small.
    mapfile -t refs < <(ostree --repo="$src" refs | grep -E "^(app/$APP_ID|runtime/$APP_ID\.Locale)/")
    [ ${#refs[@]} -gt 0 ] || { echo "no $APP_ID refs in $src" >&2; exit 1; }
    flatpak build-commit-from --src-repo="$src" "${SIGN[@]}" --no-update-summary "$DEST" "${refs[@]}"
done

flatpak build-update-repo "${SIGN[@]}" --generate-static-deltas --prune "$DEST"

rm -rf "$SITE"
mkdir -p "$SITE/repo"
for entry in "$DEST"/*; do
    case ${entry##*/} in
        tmp | state) ;;
        *) cp -r "$entry" "$SITE/repo/" ;;
    esac
done

if [ -n "$(find "$SITE" -type l -print -quit)" ]; then
    echo "symlinks in $SITE would fail the Pages upload" >&2
    exit 1
fi

cat >"$SITE/tether.flatpakrepo" <<EOF
[Flatpak Repo]
Title=Tether
Comment=Bridge your iPhone to the Linux desktop
Url=$BASE_URL/repo
Homepage=$HOMEPAGE
GPGKey=$PUBLIC_KEY
EOF

cat >"$SITE/tether.flatpakref" <<EOF
[Flatpak Ref]
Title=Tether
Name=$APP_ID
Branch=$BRANCH
Url=$BASE_URL/repo
SuggestRemoteName=tether
Homepage=$HOMEPAGE
RuntimeRepo=$RUNTIME_REPO
IsRuntime=false
GPGKey=$PUBLIC_KEY
EOF

echo "==> $SITE"
