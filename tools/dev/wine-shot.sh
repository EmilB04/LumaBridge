#!/bin/bash
# Clicks each point (optional), then saves a screenshot of the Wine window.
# Usage: tools/dev/wine-shot.sh <work dir> <name> [x y]...   -> <work dir>/shots/<name>.png
# Sidebar (1400x950 window): Dashboard 90,157  My setup 90,219  Lighting 90,281
# Games 90,343  Devices 90,405  Integrations 90,467  Settings 90,529.
# ImGui needs the mouse to rest before a click on some widgets: move, sleep 0.5, then click.
WORK=${1:?work dir}; NAME=${2:?name}; shift 2
export DISPLAY=${DISPLAY:-:99}
while [ $# -ge 2 ]; do xdotool mousemove "$1" "$2"; sleep 0.5; xdotool click 1; sleep 1.2; shift 2; done
sleep 1
import -window root -crop 1420x970+0+0 "$WORK/shots/$NAME.png"
