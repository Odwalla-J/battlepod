#!/bin/sh
# Play with a cockpit MFD beside the game: tools/bridge.py between the pod's
# Remote I/O port and tools/vmfd.py, an eight-key MFD in a window - the first
# hardware milestone with the hardware simulated. Click its keys (or press
# 1-8 in it) and they light as the game lights them; the message line and
# engine status are on its screen.
#
# usage: tools/cockpit.sh [scenario] [vehicle-record]    as tools/play.sh
# COCKPIT_LAYOUT picks a layout file (default layouts/milestone-mfd).

set -eu

python tools/bridge.py ${COCKPIT_LAYOUT:+--layout "$COCKPIT_LAYOUT"} &
BRIDGE=$!
python tools/vmfd.py &
VMFD=$!
trap 'kill $BRIDGE $VMFD 2>/dev/null || true' EXIT INT TERM

PLAY_ARGS="--rio-port 127.0.0.1:5180 ${PLAY_ARGS:-}" sh tools/play.sh "$@"
