#!/bin/bash
#
# sign-and-notarize.sh -- build the RTL8127 host app + DriverKit dext signed
# with Developer ID, notarize, staple, and produce distributable artifacts.
#
# Prerequisites (one-time, on the signing Mac):
#   1. Developer ID Application certificate in the login keychain.
#   2. Provisioning profiles installed (double-click, or drop into
#      ~/Library/Developer/Xcode/UserData/Provisioning Profiles/):
#        "RTL8127Dext Developer ID"  -> net.wizzz.RTL8127Dext, Developer ID,
#                                       with the DriverKit, DriverKit Family
#                                       Networking and DriverKit Transport PCI
#                                       entitlements
#        "RTL8127App Developer ID"   -> net.wizzz.RTL8127App, Developer ID,
#                                       with System Extension
#      Names must match PROVISIONING_PROFILE_SPECIFIER in the Xcode project.
#   3. Notarization credentials stored once:
#        xcrun notarytool store-credentials "rtl8127-notary" \
#            --apple-id <Apple ID> --team-id LRA39582TA --password <app-specific password>
#      (or export NOTARY_PROFILE to use another profile name).
#   4. Optional: a "Developer ID Installer" identity, exported as
#      DEVELOPER_ID_INSTALLER, to also produce a signed + notarized .pkg.
#
# Usage:
#   packaging/sign-and-notarize.sh [VERSION] [OUT_DIR]
#
# Output: OUT_DIR/RTL8127App-VERSION.zip (notarized, stapled app) and, when
# DEVELOPER_ID_INSTALLER is set, OUT_DIR/RTL8127-VERSION.pkg.

set -euo pipefail

VERSION="${1:-0.1.0}"
OUT="${2:-dist}"
NOTARY_PROFILE="${NOTARY_PROFILE:-rtl8127-notary}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PROJ="$HERE/RTL8127Dext/RTL8127Dext.xcodeproj"
BUILD="$HERE/RTL8127Dext/build"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# notarize FILE: submit, wait, fail (with Apple's log) unless Accepted.
notarize() {
    local file="$1" json id status
    json="$(xcrun notarytool submit "$file" --keychain-profile "$NOTARY_PROFILE" \
                --wait --output-format json)"
    id="$(printf '%s' "$json" | plutil -extract id raw -o - - 2>/dev/null || true)"
    status="$(printf '%s' "$json" | plutil -extract status raw -o - - 2>/dev/null || true)"
    echo "notarization $id: $status"
    if [ "$status" != "Accepted" ]; then
        [ -n "$id" ] && xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" || true
        echo "error: notarization of $file failed ($status)" >&2
        exit 1
    fi
}

# Build number: monotonic integer derived from the version (0.2.0 -> 200,
# 1.0.3 -> 100003). sysextd compares it to decide whether a new dext
# replaces the installed one.
BUILD_NUMBER="$(printf '%s' "$VERSION" | awk -F. '{ print $1*10000 + $2*100 + $3 }')"

echo "== Building signed Release (app + embedded dext) $VERSION ($BUILD_NUMBER)"
xcodebuild -project "$PROJ" -target RTL8127App -configuration Release \
    build \
    MARKETING_VERSION="$VERSION" \
    CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
    OTHER_CODE_SIGN_FLAGS="--timestamp" \
    RUN_CLANG_STATIC_ANALYZER=NO | tail -20

APP="$BUILD/Release/RTL8127App.app"
DEXT="$APP/Contents/Library/SystemExtensions/net.wizzz.RTL8127Dext.dext"

echo "== Verifying signatures"
codesign --verify --deep --strict --verbose=2 "$APP"
codesign -d --entitlements - "$DEXT" | grep 'com.apple.developer.driverkit' >/dev/null \
    || { echo "error: dext has no DriverKit entitlements (profile missing?)" >&2; exit 1; }
codesign -d --entitlements - "$APP" | grep 'system-extension.install' >/dev/null \
    || { echo "error: app has no system-extension.install entitlement" >&2; exit 1; }
codesign -dvv "$APP" 2>&1 | grep 'Authority=Developer ID Application' >/dev/null \
    || { echo "error: app is not signed with Developer ID" >&2; exit 1; }

ZIP="$OUT/RTL8127App-$VERSION.zip"
echo "== Notarizing app: $ZIP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
notarize "$ZIP"
xcrun stapler staple "$APP"
# Re-zip with the staple ticket inside.
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
spctl --assess --type execute --verbose=2 "$APP"
echo "Built: $ZIP"

if [ -n "${DEVELOPER_ID_INSTALLER:-}" ]; then
    PKG="$OUT/RTL8127-$VERSION.pkg"
    echo "== Building signed installer: $PKG"
    "$HERE/packaging/build-pkg.sh" "$APP" "$VERSION" "$PKG"
    notarize "$PKG"
    xcrun stapler staple "$PKG"
    spctl --assess --type install --verbose=2 "$PKG"
    echo "Built: $PKG"
fi

echo "== Done"
(cd "$OUT" && shasum -a 256 RTL8127*-"$VERSION".* )
