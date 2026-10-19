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
set -eu
umask 022
# Finder metadata and resource forks must not become AppleDouble payload entries.
# The code signatures are stored in the binaries and _CodeSignature resources.
export COPYFILE_DISABLE=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$ROOT/App/Info.plist")
OUT="$ROOT/build/release"
STAGE="$OUT/stage"
PKG="$OUT/Sona-$VERSION.pkg"
APP_IDENTITY=${SONA_APP_IDENTITY:--}
INSTALLER_IDENTITY=${SONA_INSTALLER_IDENTITY:-}
NOTARY_PROFILE=${SONA_NOTARY_PROFILE:-}
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
cp -R -X "$APP" "$R/Applications/Sona.app"
cp -R -X "$DRIVER" "$R/Library/Audio/Plug-Ins/HAL/SonaDriver.driver"
cp -X "$SERVICE" "$R/Library/PrivilegedHelperTools/SonaAudioService"
cp -X "$PLIST" "$R/Library/LaunchDaemons/com.sona.audio-service.plist"
cp -X "$ROOT/Installer/uninstall.sh" "$R/Library/Application Support/Sona/uninstall.sh"
cp -X "$ROOT/Installer/Scripts/preinstall" "$ROOT/Installer/Scripts/postinstall" "$STAGE/scripts/"
cp -R -X "$ROOT/Installer/Resources" "$STAGE/resources"
# Keep bundles writable for signing, with readable payloads and traversable directories.
chmod -R u+rwX,go+rX,go-w "$STAGE"
chmod 755 "$STAGE/scripts/preinstall" "$STAGE/scripts/postinstall" "$R/Library/Application Support/Sona/uninstall.sh" \
          "$R/Library/PrivilegedHelperTools/SonaAudioService"
chmod 644 "$R/Library/LaunchDaemons/com.sona.audio-service.plist"
find "$STAGE" \( -name .DS_Store -o -name '._*' \) -delete

echo "== signing (identity: $APP_IDENTITY)"
sign_component() {
    if [ "$APP_IDENTITY" = "-" ]; then
        codesign --force --sign - "$@"
    else
        codesign --force --options runtime --timestamp --sign "$APP_IDENTITY" "$@"
    fi
}
sign_component --identifier com.sona.audio-service "$R/Library/PrivilegedHelperTools/SonaAudioService"
sign_component --identifier com.sona.driver "$R/Library/Audio/Plug-Ins/HAL/SonaDriver.driver"
sign_component --identifier com.sona.app "$R/Applications/Sona.app"
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
pkgbuild --root "$R" --ownership recommended --component-plist "$STAGE/component.plist" --scripts "$STAGE/scripts" \
         --identifier com.sona.pkg --version "$VERSION" --install-location / "$STAGE/Sona-component-raw.pkg" >/dev/null
# macOS may add provenance to new files even when copying with -X; pkgbuild
# serializes it despite COPYFILE_DISABLE. Remove only that metadata from both
# raw cpio archives, without touching the signed payload files or scripts.
python3 "$ROOT/Installer/sanitize-package.py" "$STAGE/Sona-component-raw.pkg" "$STAGE/Sona-component.pkg"

echo "== product package"
sed "s/@VERSION@/$VERSION/g" "$ROOT/Installer/Distribution.xml" > "$STAGE/Distribution.xml"
rm -f "$PKG"
if [ -n "$INSTALLER_IDENTITY" ]; then
    productbuild --distribution "$STAGE/Distribution.xml" --resources "$STAGE/resources" \
                 --package-path "$STAGE" --sign "$INSTALLER_IDENTITY" "$PKG" >/dev/null
else
    productbuild --distribution "$STAGE/Distribution.xml" --resources "$STAGE/resources" \
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

[ "$NOTARIZED" = true ] || echo "   NOTE: not notarized; allow the package under System Settings > Privacy & Security."
