#!/usr/bin/env bash
# Builds ktplace with the GUI and bundles it with Qt into one AppImage that runs
# without Qt installed. Started with no arguments it opens the GUI; with
# arguments it is the ktplace command line.
#
# Usage: scripts/appimage/build.sh [output.AppImage]
# Needs Qt 6 (qt6-base-dev, qt6-wayland), pkg-config, wget and file.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
version=$(sed -n 's/.*version = "\(.*\)";/\1/p' src/kt_option.h)
output=$(realpath -m "${1:-ktplace-v${version}-x86_64.AppImage}")

make -j"$(nproc)" GUI=1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tools="$work/tools"
mkdir -p "$tools"
for tool in linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
            linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
    wget -q -P "$tools" "https://github.com/linuxdeploy/$tool"
done
chmod +x "$tools"/*.AppImage

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)}
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so"
export LDAI_OUTPUT="$output"
strip --strip-debug build/bin/ktplace -o "$work/ktplace"

cd "$work"
deploy() {
    PATH="$tools:$PATH" "$tools/linuxdeploy-x86_64.AppImage" --appdir AppDir "$@"
}
deploy --executable "$work/ktplace" \
    --desktop-file "$root/scripts/appimage/ktplace.desktop" \
    --icon-file "$root/scripts/appimage/ktplace.png" \
    --custom-apprun "$root/scripts/appimage/AppRun" \
    --plugin qt

plugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
cp -r "$plugins/wayland-graphics-integration-client" AppDir/usr/plugins/
deploy --deploy-deps-only AppDir/usr/plugins/wayland-graphics-integration-client --output appimage
echo "appimage: wrote $output ($(du -h "$output" | cut -f1))"
