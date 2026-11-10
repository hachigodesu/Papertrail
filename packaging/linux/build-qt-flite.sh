#!/bin/bash
# Builds Qt's flite text-to-speech engine, which Qt's own Linux builds leave out, and adds it to a Qt install, so the
# AppImage can read aloud without a speech daemon. Needs flite's and xkbcommon's headers (flite1-dev, libxkbcommon-dev).
#
# Usage: QT_ROOT=/opt/Qt/6.12.0/gcc_64 packaging/linux/build-qt-flite.sh
set -euo pipefail

qt="${QT_ROOT:?Set QT_ROOT to the Qt install, for example /opt/Qt/6.12.0/gcc_64}"
version="6.12.0"
sha256="9a60ce5bee54a9343740feab2582bffb996812f43d6c5585aade26dbd04933db"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

archive="$work/qtspeech.tar.xz"
curl -fsSL --retry 5 -o "$archive" \
    "https://download.qt.io/official_releases/qt/${version%.*}/$version/submodules/qtspeech-everywhere-src-$version.tar.xz"
echo "$sha256  $archive" | sha256sum -c -
mkdir "$work/src"
tar -xf "$archive" -C "$work/src" --strip-components=1

# It notes its options in the folder it's run from, so that's its own.
cd "$work"
"$qt/bin/qt-configure-module" "$work/src" -- -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF -B "$work/build"
cmake --build "$work/build"
plugin="$work/build/plugins/texttospeech/libqtexttospeech_flite.so"
[ -f "$plugin" ] || { echo "FAILED: the flite engine wasn't built; is flite1-dev installed?" >&2; exit 1; }
install -Dm755 "$plugin" "$qt/plugins/texttospeech/libqtexttospeech_flite.so"
echo "Added $qt/plugins/texttospeech/libqtexttospeech_flite.so"
