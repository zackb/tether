#!/bin/bash
set -e

echo "Building Tether Extensions..."

# Ensure build directories exist
BUILD_DIR="build/extension"
BROWSER_DIR="$BUILD_DIR/browser"
MAIL_DIR="$BUILD_DIR/mail"

mkdir -p "$BROWSER_DIR/src/background" "$BROWSER_DIR/src/content"
mkdir -p "$MAIL_DIR/src/background" "$MAIL_DIR/src/mail"

# Bundle Browser Extension
echo "Bundling browser extension..."
npx esbuild extension/src/background/background.js --bundle --outfile="$BROWSER_DIR/src/background/background.js"
npx esbuild extension/src/content/autofill.js --bundle --outfile="$BROWSER_DIR/src/content/autofill.js"
cp extension/manifest-browser.json "$BROWSER_DIR/manifest.json"

# Copy static assets safely
if [ -d "extension/icons" ]; then cp -R extension/icons "$BROWSER_DIR/"; fi
if [ -d "extension/popup" ]; then cp -R extension/popup "$BROWSER_DIR/"; fi

(cd "$BROWSER_DIR" && zip -qr ../tether-browser-extension.zip .)

# Bundle Mail Extension
echo "Bundling mail extension..."
npx esbuild extension/src/background/background.js --bundle --outfile="$MAIL_DIR/src/background/background.js"
npx esbuild extension/src/mail/extractor.js --bundle --outfile="$MAIL_DIR/src/mail/extractor.js"
cp extension/manifest-mail.json "$MAIL_DIR/manifest.json"

if [ -d "extension/icons" ]; then cp -R extension/icons "$MAIL_DIR/"; fi
if [ -d "extension/popup" ]; then cp -R extension/popup "$MAIL_DIR/"; fi

(cd "$MAIL_DIR" && zip -qr ../tether-mail-extension.xpi .)

echo "Extensions successfully built and packaged in $BUILD_DIR"
