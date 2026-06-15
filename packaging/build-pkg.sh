#!/bin/bash
#
# build-pkg.sh -- build a macOS installer package for the RTL8127 host app.
#
# The host app (RTL8127App.app) embeds the RTL8127Dext DriverKit extension in
# Contents/Library/SystemExtensions. The package installs the app into
# /Applications; the user then launches it once to activate the extension
# (OSSystemExtensionRequest).
#
# Usage:
#   packaging/build-pkg.sh [APP_PATH] [VERSION] [OUTPUT_PKG]
#
# Signing (optional, for a distributable package):
#   - sign the app first (Developer ID Application + DriverKit entitlements;
#     requires the entitlements granted by Apple), then
#   - set DEVELOPER_ID_INSTALLER to a "Developer ID Installer: ..." identity to
#     sign the package. Notarize + staple separately (notarytool/stapler).
#
# Without DEVELOPER_ID_INSTALLER the package is UNSIGNED: usable for development
# (SIP reduced + `systemextensionsctl developer on`), not for end users.

set -euo pipefail

APP="${1:-RTL8127Dext/build/Release/RTL8127App.app}"
VERSION="${2:-0.1.0}"
OUT="${3:-dist/RTL8127.pkg}"

PKG_IDENTIFIER="net.wizzz.RTL8127App.pkg"

if [ ! -d "$APP" ]; then
    echo "error: app bundle not found: $APP" >&2
    echo "build it first: xcodebuild -project RTL8127Dext/RTL8127Dext.xcodeproj -target RTL8127App -configuration Release build CODE_SIGNING_ALLOWED=NO" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

# Stage the app under a clean root that maps to /Applications.
ROOT="$(mktemp -d)"
COMPONENT="$(mktemp -d)/RTL8127App-component.pkg"
trap 'rm -rf "$ROOT" "$(dirname "$COMPONENT")"' EXIT

cp -R "$APP" "$ROOT/"

# Component package: the app, installed to /Applications.
pkgbuild --root "$ROOT" \
         --install-location /Applications \
         --identifier "$PKG_IDENTIFIER" \
         --version "$VERSION" \
         "$COMPONENT"

# Product archive (double-clickable installer), optionally signed.
if [ -n "${DEVELOPER_ID_INSTALLER:-}" ]; then
    echo "Signing package with: $DEVELOPER_ID_INSTALLER"
    productbuild --package "$COMPONENT" --sign "$DEVELOPER_ID_INSTALLER" "$OUT"
else
    echo "WARNING: building an UNSIGNED package (set DEVELOPER_ID_INSTALLER to sign)."
    productbuild --package "$COMPONENT" "$OUT"
fi

echo "Built: $OUT"
