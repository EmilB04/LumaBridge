#!/bin/bash
# Starts the mingw-built LumaBridge.exe in Wine on a virtual display, for looking at the UI
# from Linux. Usage: tools/dev/wine-run.sh <work dir>   (the build is <work dir>/build/x64)
# Extra environment for the app can be passed in, e.g. LUMA_DEMO=1 for a staging patch.
set -e
WORK=${1:?work dir}
export DISPLAY=${DISPLAY:-:99} WINEPREFIX=$WORK/wineprefix WINEDEBUG=-all
pgrep -x Xvfb >/dev/null || (nohup Xvfb "$DISPLAY" -screen 0 1600x1000x24 >/dev/null 2>&1 &)
for _ in $(seq 1 30); do xdotool getdisplaygeometry >/dev/null 2>&1 && break; sleep 0.5; done
wineserver -k 2>/dev/null || true
sleep 2
# A killed instance looks like a crash to the next start (it then pauses the lighting).
INI=$WINEPREFIX/drive_c/users/root/AppData/Local/LumaBridge/LumaBridge.ini
[ -f "$INI" ] && sed -i 's/^Running=1/Running=0/' "$INI"
mkdir -p "$WORK/shots"
cd "$WORK/build/x64" && (timeout 1800 wine ./LumaBridge.exe > "$WORK/wine.log" 2>&1 &)
# Wait for the window (the first start in a fresh prefix takes longer).
W=
for _ in $(seq 1 90); do
    W=$(xdotool search --name "LumaBridge" 2>/dev/null | head -1)
    [ -n "$W" ] && break
    sleep 1
done
[ -n "$W" ] || { echo "no LumaBridge window; see $WORK/wine.log" >&2; exit 1; }
sleep 3
xdotool windowsize "$W" 1400 950
sleep 2
