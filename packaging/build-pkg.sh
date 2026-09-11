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
trap 'rm -rf "$ROOT" "$(dirname "$COMPONENT")" "$SCRIPTS"' EXIT
SCRIPTS=""

cp -R "$APP" "$ROOT/"

# postinstall: open the app for the logged-in user so the driver activation
# (and the one-time approval prompt) happens right after the install.
SCRIPTS="$(mktemp -d)"
cat > "$SCRIPTS/postinstall" <<'POST'
#!/bin/bash
user="$(stat -f %Su /dev/console 2>/dev/null)"
if [ -n "$user" ] && [ "$user" != "root" ]; then
    # A running copy of the previous app would just come to front on
    # "open" and never submit the new driver: quit it first.
    sudo -u "$user" osascript -e 'tell application "RTL8127App" to quit' >/dev/null 2>&1 || true
    sleep 1
    sudo -u "$user" open "/Applications/RTL8127App.app" || true
fi
exit 0
POST
chmod +x "$SCRIPTS/postinstall"

# Component package: the app, installed to /Applications.
pkgbuild --root "$ROOT" \
         --install-location /Applications \
         --identifier "$PKG_IDENTIFIER" \
         --version "$VERSION" \
         --scripts "$SCRIPTS" \
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
