#!/bin/sh
# Remove the Brother HL-1210W native driver.
#
#   sudo sh uninstall.sh
#
# Print queues are left alone; delete them in System Settings > Printers &
# Scanners (or with "lpadmin -x <queue>") if you no longer want them.

set -e

VENDOR_DIR="/Library/Printers/HL1210W-Native"
PPD="/Library/Printers/PPDs/Contents/Resources/Brother HL-1210W series native.gz"
PKG_ID="org.hl1210w.native-driver"

if [ "$(id -u)" -ne 0 ]; then
    echo "This needs administrator rights. Run: sudo sh \"$0\"" >&2
    exit 1
fi

rm -f "$PPD" "$(dirname "$PPD")/._$(basename "$PPD")"
rm -rf "$VENDOR_DIR"
pkgutil --forget "$PKG_ID" >/dev/null 2>&1 || true

echo "The HL-1210W native driver has been removed."

QUEUES=$(lpstat -v 2>/dev/null | grep -i "HL-1210W" | sed -e 's/^device for //' -e 's/:.*//' || true)
if [ -n "$QUEUES" ]; then
    echo "These print queues still exist and may need to be deleted or given"
    echo "another driver:"
    echo "$QUEUES" | sed 's/^/  /'
fi
