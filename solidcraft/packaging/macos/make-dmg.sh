#!/bin/sh
# Packs the macOS builds as "NCX SolidCraft.app" (Apple silicon and Intel in one program) in a disk image:
# solidcraft/out/NCX-SolidCraft-<version>-macos.dmg
# Run from the repository root on a Mac, after
#   cargo build --release --target aarch64-apple-darwin && cargo build --release --target x86_64-apple-darwin
# in solidcraft/. The app is signed ad hoc only (no Apple Developer ID): the first time, macOS asks the user to
# open it with right-click > Open.
set -eu
VER=$(sed -n 's/^version *= *"\([0-9.]*\)".*/\1/p' solidcraft/Cargo.toml | head -n 1)
OUT=solidcraft/out
APP="$OUT/NCX SolidCraft.app"
rm -rf "$APP" "$OUT/dmg" "$OUT/icon.iconset"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

lipo -create \
    solidcraft/target/aarch64-apple-darwin/release/ncx-solidcraft \
    solidcraft/target/x86_64-apple-darwin/release/ncx-solidcraft \
    -output "$APP/Contents/MacOS/ncx-solidcraft"

# the icon, from the 512 px picture
mkdir -p "$OUT/icon.iconset"
for n in 16 32 128 256 512; do
    sips -z $n $n solidcraft/assets/icon-512.png --out "$OUT/icon.iconset/icon_${n}x${n}.png" >/dev/null
done
for n in 16 32 128 256; do
    d=$((n * 2))
    sips -z $d $d solidcraft/assets/icon-512.png --out "$OUT/icon.iconset/icon_${n}x${n}@2x.png" >/dev/null
done
iconutil -c icns "$OUT/icon.iconset" -o "$APP/Contents/Resources/icon.icns"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>NCX SolidCraft</string>
  <key>CFBundleDisplayName</key><string>NCX SolidCraft</string>
  <key>CFBundleIdentifier</key><string>io.github.simonrafferty.ncx-solidcraft</string>
  <key>CFBundleExecutable</key><string>ncx-solidcraft</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
cp LICENSE "$APP/Contents/Resources/LICENSE.txt"
cp solidcraft/assets/fonts/OFL.txt "$APP/Contents/Resources/IBM-Plex-OFL.txt"

# ad-hoc signature (Apple silicon runs only signed code)
codesign --force --deep --sign - "$APP"

mkdir -p "$OUT/dmg"
cp -R "$APP" "$OUT/dmg/"
ln -s /Applications "$OUT/dmg/Applications"
hdiutil create -volname "NCX SolidCraft" -srcfolder "$OUT/dmg" -ov -format UDZO "$OUT/NCX-SolidCraft-$VER-macos.dmg"
echo "built $OUT/NCX-SolidCraft-$VER-macos.dmg"
