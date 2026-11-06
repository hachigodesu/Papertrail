#!/bin/sh
# Checks that an AppImage starts on this distro, using its bundled Qt.
#
# Usage: smoke-test-appimage.sh Papertrail-x86_64.AppImage
#
# Containers have no FUSE, so this uses the AppImage's extract-and-run fallback.
set -eu

image="$(realpath "$1")"
chmod +x "$image"
export APPIMAGE_EXTRACT_AND_RUN=1
export QT_QPA_PLATFORM=offscreen

grep PRETTY_NAME /etc/os-release
ldd --version 2>&1 | head -1

# A GUI app runs until stopped, so being stopped by the timeout (124) is the pass.
status=0
timeout 15 "$image" || status=$?
if [ "$status" -ne 124 ]; then
    echo "FAILED: the app exited early with status $status"
    exit 1
fi
echo "PASSED: the app kept running until stopped"
