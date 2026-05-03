#!/bin/bash
set -e

SOURCE="src/gtk/tether.png"

if [ ! -f "$SOURCE" ]; then
    echo "Source image $SOURCE not found!"
    exit 1
fi

echo "Generating extension icons..."
mkdir -p extension/icons
for size in 16 48 128; do
    convert "$SOURCE" -resize "${size}x${size}" "extension/icons/icon${size}.png"
done

echo "Generating Apple icons..."
APPLE_DIR="apple/Tether/Assets.xcassets/AppIcon.appiconset"
mkdir -p "$APPLE_DIR"
convert "$SOURCE" -resize 1024x1024 "$APPLE_DIR/Image.png"

for size in 16 32 128 256 512; do
    convert "$SOURCE" -resize "${size}x${size}" "$APPLE_DIR/icon_${size}x${size}.png"
    double_size=$((size * 2))
    convert "$SOURCE" -resize "${double_size}x${double_size}" "$APPLE_DIR/icon_${size}x${size}@2x.png"
done

cat > "$APPLE_DIR/Contents.json" <<EOF
{
  "images" : [
    {
      "filename" : "Image.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "appearances" : [
        {
          "appearance" : "luminosity",
          "value" : "dark"
        }
      ],
      "filename" : "Image.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "appearances" : [
        {
          "appearance" : "luminosity",
          "value" : "tinted"
        }
      ],
      "filename" : "Image.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "filename" : "icon_16x16.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "16x16"
    },
    {
      "filename" : "icon_16x16@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "16x16"
    },
    {
      "filename" : "icon_32x32.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "32x32"
    },
    {
      "filename" : "icon_32x32@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "32x32"
    },
    {
      "filename" : "icon_128x128.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "128x128"
    },
    {
      "filename" : "icon_128x128@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "128x128"
    },
    {
      "filename" : "icon_256x256.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "256x256"
    },
    {
      "filename" : "icon_256x256@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "256x256"
    },
    {
      "filename" : "icon_512x512.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "512x512"
    },
    {
      "filename" : "icon_512x512@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "512x512"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
EOF

echo "Generating GTK icons..."
GTK_ICON_DIR="src/gtk/icons"
mkdir -p "$GTK_ICON_DIR"
for size in 16 32 48 64 128 256 512 1024; do
    mkdir -p "$GTK_ICON_DIR/${size}x${size}"
    convert "$SOURCE" -resize "${size}x${size}" "$GTK_ICON_DIR/${size}x${size}/tether.png"
done

echo "All icons generated successfully."
