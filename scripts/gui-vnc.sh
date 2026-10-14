#!/usr/bin/env bash
# Open ktplace-gui on the dev container's VNC display, replacing a window that
# is already there.
#
#   scripts/gui-vnc.sh [ktplace --gui args]
#
# Run inside the dev container; scripts/devenv.sh gui does it from the host and
# then opens the viewer. The window is `ktplace --gui "$@"` on display :1, which
# the container starts when it boots (docker/dev-entrypoint.sh) and serves over
# VNC on 127.0.0.1:5901 on the host.
#
# A container created from an image older than that has no display yet; this
# starts one the same way, so the script works there too.
#
# Environment (used only when the display has to be started here; otherwise
# they were fixed when the container started -- see compose.yaml):
#   KTPLACE_GUI_SIZE      window size in points (default 1440x900)
#   KTPLACE_VNC_PASSWORD  VNC password (default "ktplace"; "" for none). macOS
#                         Screen Sharing will not connect without one.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
export DISPLAY=:1

if ! pgrep -x Xvfb >/dev/null; then
    Xvfb :1 -screen 0 "${KTPLACE_GUI_SIZE:-1440x900}x24" -nolisten tcp -ac \
        >/tmp/ktplace-xvfb.log 2>&1 &
    sleep 1
fi
if ! pgrep -x x11vnc >/dev/null; then
    auth=(-nopw)
    password="${KTPLACE_VNC_PASSWORD-ktplace}"
    if [ -n "$password" ]; then
        x11vnc -storepasswd "$password" /tmp/ktplace-vnc.pass >/dev/null 2>&1
        auth=(-rfbauth /tmp/ktplace-vnc.pass)
    fi
    x11vnc -display :1 -forever -shared -quiet -rfbport 5900 "${auth[@]}" \
        -bg -o /tmp/ktplace-x11vnc.log >/dev/null 2>&1
fi

pkill -x ktplace-gui 2>/dev/null || true
sleep 0.5
nohup build/bin/ktplace --gui -qwindowgeometry "${KTPLACE_GUI_SIZE:-1440x900}+0+0" "$@" \
    >/tmp/ktplace-gui.log 2>&1 &
echo "ktplace-gui started (pid $!); its stderr is in /tmp/ktplace-gui.log"
