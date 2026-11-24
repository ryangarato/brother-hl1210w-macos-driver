#!/bin/sh
# Create a print queue for a Brother HL-1210W that uses the native driver.
#
#   sh add-printer.sh                 find the printer on the network or USB
#   sh add-printer.sh <device-uri>    use a specific address, for example
#                                     socket://192.168.1.50  or  lpd://192.168.1.50/BINARY_P1
#
# The driver must be installed first.  You can do the same thing by hand in
# System Settings > Printers & Scanners > Add Printer: pick the printer, then
# under "Use" choose "Select Software..." and
# "Brother HL-1210W series - native open driver".

set -e

PPD="/Library/Printers/PPDs/Contents/Resources/Brother HL-1210W series native.gz"
QUEUE="${HL1210W_QUEUE:-Brother_HL_1210W}"
URI="$1"

if [ ! -f "$PPD" ]; then
    echo "The driver is not installed yet ($PPD is missing)." >&2
    exit 1
fi

if [ -z "$URI" ]; then
    echo "Looking for the printer (about 15 seconds)..."
    URI=$(lpinfo --timeout 15 -v 2>/dev/null |
          awk 'tolower($0) ~ /hl-1210w/ { print $2; exit }')
fi

if [ -z "$URI" ]; then
    echo "No HL-1210W was found. Make sure it is switched on and on the same" >&2
    echo "network (or plugged in by USB), or pass its address:" >&2
    echo "  sh $0 socket://<printer-ip-address>" >&2
    exit 1
fi

# Default paper: Letter where Letter is the everyday size, A4 elsewhere.
REGION=$(defaults read -g AppleLocale 2>/dev/null | sed -e 's/@.*//' -e 's/.*_//')
case "$REGION" in
    US|CA|MX|PH|PR|CL|CO|CR|DO|GT|PA|SV|VE) PAPER="Letter" ;;
    *)                                      PAPER="A4" ;;
esac

echo "Adding queue \"$QUEUE\" for $URI (default paper: $PAPER)"
lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD" \
        -D "Brother HL-1210W" -o printer-is-shared=false -o PageSize="$PAPER" 2>&1 |
    grep -v "Printer drivers are deprecated" || true

if lpstat -p "$QUEUE" >/dev/null 2>&1; then
    echo "Done. \"Brother HL-1210W\" is now available in every print dialog."
else
    echo "The queue could not be created." >&2
    exit 1
fi
