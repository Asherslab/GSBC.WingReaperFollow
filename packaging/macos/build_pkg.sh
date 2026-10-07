#!/bin/sh
# Builds a macOS installer package for WING Follow.
#   packaging/macos/build_pkg.sh <path/to/reaper_wingfollow.dylib> <version> <output dir>
#
# The package installs for the current user only, into
#   ~/Library/Application Support/REAPER/UserPlugins/
# so no admin password is needed. Files placed by Installer are not quarantined, which avoids
# the Gatekeeper "damaged / can't be opened" problem a downloaded .dylib has.
set -eu

DYLIB="$1"
VERSION="$2"
OUT="$3"
ID="com.wingfollow.reaper"
HERE="$(cd "$(dirname "$0")" && pwd)"

# pkg versions must be numeric: "1.2.3-rc1" -> "1.2.3"
PKG_VERSION="$(printf '%s' "$VERSION" | sed -E 's/^v//; s/[^0-9.].*$//')"
[ -n "$PKG_VERSION" ] || PKG_VERSION="0.0.0"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ROOT="$WORK/root/Library/Application Support/REAPER/UserPlugins"
mkdir -p "$ROOT" "$OUT"
cp "$DYLIB" "$ROOT/reaper_wingfollow.dylib"
chmod 755 "$ROOT/reaper_wingfollow.dylib"
# Ad-hoc signature (Apple Silicon refuses unsigned code); a no-op if already signed.
codesign --force --sign - "$ROOT/reaper_wingfollow.dylib"

# With the current-user-home domain, the payload's paths are relative to the user's home.
pkgbuild --root "$WORK/root" --identifier "$ID" --version "$PKG_VERSION" \
  --install-location "/" "$WORK/component.pkg"

sed -e "s/@VERSION@/$PKG_VERSION/g" -e "s/@DISPLAY_VERSION@/$VERSION/g" -e "s/@ID@/$ID/g" \
  "$HERE/distribution.xml" > "$WORK/distribution.xml"
mkdir -p "$WORK/resources"
sed -e "s/@DISPLAY_VERSION@/$VERSION/g" "$HERE/welcome.html" > "$WORK/resources/welcome.html"
cp "$HERE/conclusion.html" "$WORK/resources/conclusion.html"

PKG="$OUT/WING-Follow-$VERSION-macOS.pkg"
productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/resources" \
  --package-path "$WORK" "$PKG"
echo "Built $PKG"
