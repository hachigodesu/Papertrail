#!/bin/bash
# Builds a pinned MuPDF and installs it to /usr/local, for distros whose own package is older.
#
# Usage: install-mupdf.sh [CACHE_DIR]
#
# If CACHE_DIR already holds a build it's reused, which saves CI several minutes. Otherwise the
# new build is left there for next time. Run as root.
set -euo pipefail

version=1.28.5
cache="${1:-$(mktemp -d)}"
flags=(build=release shared=yes prefix=/usr/local
       HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_LEPTONICA=no HAVE_TESSERACT=no)

if [ ! -f "$cache/usr/local/include/mupdf/fitz.h" ]; then
    work="$(mktemp -d)"
    curl -fsSL --retry 5 -o "$work/mupdf.tar.gz" \
        "https://casper.mupdf.com/downloads/archive/mupdf-$version-source.tar.gz"
    tar xf "$work/mupdf.tar.gz" -C "$work"
    make -C "$work/mupdf-$version-source" -j"$(nproc)" "${flags[@]}" libs > "$work/build.log" 2>&1 \
        || { tail -30 "$work/build.log"; exit 1; }
    make -C "$work/mupdf-$version-source" "${flags[@]}" DESTDIR="$cache" install-libs >> "$work/build.log" 2>&1 \
        || { tail -30 "$work/build.log"; exit 1; }
    rm -rf "$work"
fi

cp -a "$cache/usr/local/." /usr/local/
ldconfig
