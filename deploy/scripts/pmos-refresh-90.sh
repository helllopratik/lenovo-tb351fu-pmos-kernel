#!/bin/sh
# Switch the panel to the stock 90 Hz timing after boot.
#
# pmosfb keeps the bootloader 60 Hz state by default so a bad 90 Hz boot
# is still recoverable.  Writing the parameter changes only the DSI VFP
# (1104 -> 26) and the synthetic vblank rate; the panel's pixel clock and
# horizontal timing are unchanged.  Removing/disable this unit returns the
# next boot to 60 Hz.
set -e

P=/sys/module/pmosfb/parameters/refresh_hz
[ -e "$P" ] || exit 0

echo 90 > "$P"
