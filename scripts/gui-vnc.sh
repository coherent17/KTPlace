#!/usr/bin/env bash
# Serve ktplace-gui over VNC from a container: a virtual X display (Xvfb), the
# window on it, and x11vnc publishing that display on port 5900.
#
#   scripts/gui-vnc.sh [ktplace-gui args]
#
# Run inside the dev container; scripts/devenv.sh gui does it for you. A window
# already running is replaced, and the display and the VNC server are reused.
#
# Not Qt's own VNC platform (-platform vnc): it crashed with bad_alloc when the
# macOS TigerVNC viewer connected. x11vnc is a full VNC server and takes any
# viewer.
#
# Environment:
#   KTPLACE_GUI_SIZE    window size in points (default 1440x900)
#   KTPLACE_GUI_SCALE   pixels per point (default 2). A Retina Mac's viewer
#                       shows one remote pixel per physical pixel, so at 1 the
#                       window comes out at half size; 2 matches it.
#   KTPLACE_VNC_PASSWORD  require this password (default: none). macOS Screen
#                       Sharing will not connect without one; TigerVNC will.
set -euo pipefail

size="${KTPLACE_GUI_SIZE:-1440x900}"
scale="${KTPLACE_GUI_SCALE:-2}"
width="${size%x*}"
height="${size#*x}"
display=:1

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if ! pgrep -x Xvfb >/dev/null; then
    Xvfb "$display" -screen 0 "$((width * scale))x$((height * scale))x24" -nolisten tcp \
        >/tmp/ktplace-xvfb.log 2>&1 &
    sleep 1
fi

if ! pgrep -x x11vnc >/dev/null; then
    auth=(-nopw)
    if [ -n "${KTPLACE_VNC_PASSWORD:-}" ]; then
        x11vnc -storepasswd "$KTPLACE_VNC_PASSWORD" /tmp/ktplace-vnc.pass >/dev/null 2>&1
        auth=(-rfbauth /tmp/ktplace-vnc.pass)
    fi
    x11vnc -display "$display" -forever -shared -quiet -rfbport 5900 "${auth[@]}" \
        -bg -o /tmp/ktplace-x11vnc.log >/dev/null 2>&1
fi

pkill -x ktplace-gui 2>/dev/null || true
sleep 0.5

export DISPLAY="$display"
export QT_SCALE_FACTOR="$scale"
nohup build/bin/ktplace-gui -platform xcb -qwindowgeometry "${size}+0+0" "$@" \
    >/tmp/ktplace-gui.log 2>&1 &
echo "ktplace-gui started (pid $!); its stderr is in /tmp/ktplace-gui.log"
