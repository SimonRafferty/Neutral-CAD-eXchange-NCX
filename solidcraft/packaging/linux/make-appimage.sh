#!/bin/sh
# Packs the Linux build as one AppImage: solidcraft/out/NCX-SolidCraft-<version>-x86_64.AppImage
# Run from the repository root on Linux (x86_64), after `cargo build --release` in solidcraft/.
# Uses appimagetool (downloaded here if it is not on PATH); --appimage-extract-and-run, so no FUSE is needed.
set -eu
VER=$(sed -n 's/^version *= *"\([0-9.]*\)".*/\1/p' solidcraft/Cargo.toml | head -n 1)
OUT=solidcraft/out
APPDIR=$OUT/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/doc/ncx-solidcraft"

cp solidcraft/target/release/ncx-solidcraft "$APPDIR/usr/bin/"
cp solidcraft/assets/icon-512.png "$APPDIR/ncx-solidcraft.png"
cp LICENSE "$APPDIR/usr/share/doc/ncx-solidcraft/LICENSE"
cp solidcraft/assets/fonts/OFL.txt "$APPDIR/usr/share/doc/ncx-solidcraft/IBM-Plex-OFL.txt"

cat > "$APPDIR/ncx-solidcraft.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=NCX SolidCraft
Comment=NCX files to SolidCraft designs and back
Exec=ncx-solidcraft %f
Icon=ncx-solidcraft
Categories=Graphics;Engineering;
Terminal=false
EOF

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/ncx-solidcraft" "$@"
EOF
chmod +x "$APPDIR/AppRun" "$APPDIR/usr/bin/ncx-solidcraft"

TOOL=$(command -v appimagetool || true)
if [ -z "$TOOL" ]; then
    # kept out of $OUT, so it is never taken for the program's own AppImage
    TOOL=${TMPDIR:-/tmp}/appimagetool-x86_64.AppImage
    [ -f "$TOOL" ] || curl -sSfL -o "$TOOL" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi
ARCH=x86_64 "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUT/NCX-SolidCraft-$VER-x86_64.AppImage"
echo "built $OUT/NCX-SolidCraft-$VER-x86_64.AppImage"
