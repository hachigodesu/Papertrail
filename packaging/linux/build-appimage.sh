#!/bin/bash
# Builds Papertrail-<arch>.AppImage with Qt bundled inside.
#
# Usage: QT_ROOT=/opt/Qt/6.12.0/gcc_64 packaging/linux/build-appimage.sh [OUTPUT_DIR]
#
# Build on the oldest distro you want to support: the AppImage won't run on a
# system with an older glibc than the one it was built on.
set -euo pipefail

src="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$PWD/appimage-output}"
qt="${QT_ROOT:?Set QT_ROOT to the Qt install, for example /opt/Qt/6.12.0/gcc_64}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

tools="${APPIMAGE_TOOLS_DIR:-$HOME/.cache/papertrail-appimage-tools}"
mkdir -p "$tools" "$out"
for tool in \
    linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
    linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
do
    file="$tools/$(basename "$tool")"
    [ -x "$file" ] || { curl -fsSL --retry 5 -o "$file" "https://github.com/$tool"; chmod +x "$file"; }
done

cmake -S "$src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$qt" -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$work/build"
DESTDIR="$work/AppDir" cmake --install "$work/build"

export PATH="$tools:$qt/bin:$PATH"
export QMAKE="$qt/bin/qmake"
export LD_LIBRARY_PATH="$qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export APPIMAGE_EXTRACT_AND_RUN=1          # The tools themselves shouldn't need FUSE.
# Offscreen is bundled so that smoke tests can run without a display.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
export EXTRA_QT_MODULES="waylandclient"
export LDAI_OUTPUT="$out/Papertrail-x86_64.AppImage"
export OUTPUT="$LDAI_OUTPUT"

# Qt Network brings in Kerberos and bundles most of it, but linuxdeploy leaves out libcom_err because it
# assumes every system has it. A minimal openSUSE doesn't, and then the app can't start at all.
com_err="$(ldconfig -p | awk '/libcom_err\.so\.2 / {print $NF; exit}')"
[ -n "$com_err" ] || { echo "FAILED: libcom_err.so.2 isn't installed here" >&2; exit 1; }

cd "$work"
linuxdeploy-x86_64.AppImage \
    --appdir "$work/AppDir" \
    --desktop-file "$src/packaging/linux/io.github.hachigodesu.Papertrail.desktop" \
    --icon-file "$src/packaging/linux/io.github.hachigodesu.Papertrail.svg" \
    --executable "$work/AppDir/usr/bin/papertrail" \
    --executable "$work/AppDir/usr/bin/papertrail-worker" \
    --library "$com_err" \
    --plugin qt \
    --output appimage

if [ ! -e "$work/AppDir/usr/lib/libcom_err.so.2" ]; then
    echo "FAILED: the AppImage has no libcom_err" >&2
    exit 1
fi

# Online lookups can't work without this, and nothing else would notice it was missing.
if ! ls "$work/AppDir/usr/plugins/tls/"*.so >/dev/null 2>&1; then
    echo "FAILED: the AppImage has no TLS plugin" >&2
    exit 1
fi

echo "Built: $LDAI_OUTPUT"
