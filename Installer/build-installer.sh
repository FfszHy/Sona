#!/bin/sh
# Builds the Sona installer package from the already-built driver, service and app.
#
#   make release                         -> build/release/Sona-<version>.pkg (+ .sha256)
#
# Without credentials everything is ad-hoc signed and the package is unsigned: macOS blocks the
# first launch until the user allows it under System Settings > Privacy & Security. With an
# Apple Developer Program membership set these and the same command produces a notarized package:
#
#   SONA_APP_IDENTITY="Developer ID Application: Name (TEAMID)"
#   SONA_INSTALLER_IDENTITY="Developer ID Installer: Name (TEAMID)"
#   SONA_NOTARY_PROFILE=<profile from `xcrun notarytool store-credentials`>
#
# Optional: SONA_DOWNLOAD_BASE (URL prefix written into the website, default /downloads),
# SONA_SOURCE_URL (public source repository, default none).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$ROOT/App/Info.plist")
OUT="$ROOT/build/release"
STAGE="$OUT/stage"
PKG="$OUT/Sona-$VERSION.pkg"
APP_IDENTITY=${SONA_APP_IDENTITY:--}
INSTALLER_IDENTITY=${SONA_INSTALLER_IDENTITY:-}
NOTARY_PROFILE=${SONA_NOTARY_PROFILE:-}
DOWNLOAD_BASE=${SONA_DOWNLOAD_BASE:-/downloads}
SOURCE_URL=${SONA_SOURCE_URL:-}
APP="$ROOT/App/build/Sona.app"
DRIVER="$ROOT/Driver/build/SonaDriver.driver"
SERVICE="$ROOT/Service/build/SonaAudioService"
PLIST="$ROOT/Service/com.sona.audio-service.plist"
for f in "$APP" "$DRIVER" "$SERVICE" "$PLIST"; do [ -e "$f" ] || { echo "missing $f (run make first)"; exit 1; }; done
for v in App Driver; do
    got=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$ROOT/$v/Info.plist")
    [ "$got" = "$VERSION" ] || { echo "$v/Info.plist says $got, App says $VERSION"; exit 1; }
done
grep -q "\"$VERSION\"" "$ROOT/Shared/SonaProtocol.h" || { echo "Shared/SonaProtocol.h kSonaDriverVersion is not $VERSION"; exit 1; }

echo "== staging Sona $VERSION"
rm -rf "$STAGE"
R="$STAGE/root"
mkdir -p "$R/Applications" "$R/Library/Audio/Plug-Ins/HAL" "$R/Library/LaunchDaemons" \
         "$R/Library/PrivilegedHelperTools" "$R/Library/Application Support/Sona" "$STAGE/scripts"
cp -R "$APP" "$R/Applications/Sona.app"
cp -R "$DRIVER" "$R/Library/Audio/Plug-Ins/HAL/SonaDriver.driver"
cp "$SERVICE" "$R/Library/PrivilegedHelperTools/SonaAudioService"
cp "$PLIST" "$R/Library/LaunchDaemons/com.sona.audio-service.plist"
cp "$ROOT/Installer/uninstall.sh" "$R/Library/Application Support/Sona/uninstall.sh"
cp "$ROOT/Installer/Scripts/preinstall" "$ROOT/Installer/Scripts/postinstall" "$STAGE/scripts/"
chmod 755 "$STAGE/scripts/preinstall" "$STAGE/scripts/postinstall" "$R/Library/Application Support/Sona/uninstall.sh" \
          "$R/Library/PrivilegedHelperTools/SonaAudioService"
chmod 644 "$R/Library/LaunchDaemons/com.sona.audio-service.plist"
find "$R" -name .DS_Store -delete

echo "== signing (identity: $APP_IDENTITY)"
if [ "$APP_IDENTITY" = "-" ]; then
    SIGN="codesign --force --sign -"
else
    SIGN="codesign --force --options runtime --timestamp --sign $APP_IDENTITY"
fi
$SIGN --identifier com.sona.audio-service "$R/Library/PrivilegedHelperTools/SonaAudioService"
$SIGN --identifier com.sona.driver "$R/Library/Audio/Plug-Ins/HAL/SonaDriver.driver"
$SIGN --identifier com.sona.app "$R/Applications/Sona.app"
codesign --verify --strict "$R/Applications/Sona.app" "$R/Library/Audio/Plug-Ins/HAL/SonaDriver.driver" "$R/Library/PrivilegedHelperTools/SonaAudioService"

echo "== component package"
# Bundles must land exactly where the payload says; never let Installer "relocate" the app onto a
# copy that happens to exist elsewhere (a build directory, the Desktop).
pkgbuild --analyze --root "$R" "$STAGE/component.plist" >/dev/null
python3 - "$STAGE/component.plist" <<'EOF'
import plistlib, sys
path = sys.argv[1]
with open(path, "rb") as f: entries = plistlib.load(f)
for e in entries:
    e["BundleIsRelocatable"] = False
    e["BundleIsVersionChecked"] = False
    e["BundleOverwriteAction"] = "upgrade"
with open(path, "wb") as f: plistlib.dump(entries, f)
EOF
pkgbuild --root "$R" --component-plist "$STAGE/component.plist" --scripts "$STAGE/scripts" \
         --identifier com.sona.pkg --version "$VERSION" --install-location / "$STAGE/Sona-component.pkg" >/dev/null

echo "== product package"
sed "s/@VERSION@/$VERSION/g" "$ROOT/Installer/Distribution.xml" > "$STAGE/Distribution.xml"
rm -f "$PKG"
if [ -n "$INSTALLER_IDENTITY" ]; then
    productbuild --distribution "$STAGE/Distribution.xml" --resources "$ROOT/Installer/Resources" \
                 --package-path "$STAGE" --sign "$INSTALLER_IDENTITY" "$PKG" >/dev/null
else
    productbuild --distribution "$STAGE/Distribution.xml" --resources "$ROOT/Installer/Resources" \
                 --package-path "$STAGE" "$PKG" >/dev/null
fi

NOTARIZED=false
if [ -n "$NOTARY_PROFILE" ]; then
    echo "== notarizing"
    xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG"
    NOTARIZED=true
fi

SHA=$(shasum -a 256 "$PKG" | cut -d' ' -f1)
SIZE=$(stat -f %z "$PKG")
echo "$SHA  Sona-$VERSION.pkg" > "$PKG.sha256"
echo "== $PKG ($SIZE bytes)"
echo "   sha256 $SHA"
pkgutil --check-signature "$PKG" | head -3 | sed 's/^/   /'
installer -pkginfo -pkg "$PKG" | sed 's/^/   installs: /'

echo "== website"
DL="$ROOT/website/public/downloads"
mkdir -p "$DL"
rm -f "$DL"/Sona-*.pkg "$DL"/Sona-*.pkg.sha256
cp "$PKG" "$PKG.sha256" "$DL/"
DATE=$(date +%Y-%m-%d)
SOURCE_TS=null; [ -n "$SOURCE_URL" ] && SOURCE_TS="'$SOURCE_URL'"
cat > "$ROOT/website/src/release.ts" <<EOF
// Generated by Installer/build-installer.sh (make release). Do not edit by hand.
export const release = {
  version: '$VERSION' as string | null,
  fileName: 'Sona-$VERSION.pkg',
  downloadUrl: '$DOWNLOAD_BASE/Sona-$VERSION.pkg' as string | null,
  sha256: '$SHA',
  sizeBytes: $SIZE,
  releasedAt: '$DATE',
  notarized: $NOTARIZED,
  minMacOS: '26',
  sourceUrl: $SOURCE_TS as string | null,
};
EOF
node -e '
const fs = require("fs"), p = process.argv[1], v = process.argv[2], base = process.argv[3];
const j = JSON.parse(fs.readFileSync(p, "utf8"));
j.redirects = (j.redirects || []).filter(r => r.source !== "/download");
j.redirects.unshift({ source: "/download", destination: `${base}/Sona-${v}.pkg`, permanent: false });
fs.writeFileSync(p, JSON.stringify(j, null, 2) + "\n");
' "$ROOT/website/vercel.json" "$VERSION" "$DOWNLOAD_BASE"
echo "   website/public/downloads/Sona-$VERSION.pkg, website/src/release.ts and /download redirect updated"
[ "$NOTARIZED" = true ] || echo "   NOTE: not notarized; the site tells users how to allow the package under Privacy & Security."
