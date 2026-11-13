#!/bin/bash
# Builds Papertrail's Windows installer and portable zip.
#
# Usage, in an MSYS2 UCRT64 shell with NSIS installed (pacman -S mingw-w64-ucrt-x86_64-nsis):
#   packaging/windows/build-package.sh [OUTPUT_DIR]
#
# Makes Papertrail-<version>-setup.exe and Papertrail-<version>-portable.zip. Both hold the same files: the
# program, every DLL and Qt plugin it needs from MSYS2, and the licence texts of all of them. Before packing,
# the program's --self-test is run with nothing from MSYS2 on PATH, so a missing DLL fails the build rather
# than a user's first start.
set -euo pipefail

src="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(realpath -m "${1:-$src/build/package-output}")"
build="$src/build/package"
ucrt="$(cd "$(dirname "$(command -v g++)")/.." && pwd)"
[ -x "$ucrt/bin/makensis.exe" ] || { echo "FAILED: NSIS isn't installed. Run: pacman -S mingw-w64-ucrt-x86_64-nsis" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$out"

if [ -n "${PAPERTRAIL_CONFIGURE_WITHOUT_PRESET:-}" ]; then
    # CI's MSYS2 isn't at C:/msys64, which the preset names.
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DPAPERTRAIL_PACKAGE=ON
else
    cmake --preset windows-release -B "$build" -DBUILD_TESTING=OFF -DPAPERTRAIL_PACKAGE=ON
fi
cmake --build "$build"
version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")"
cmake --install "$build" --prefix "$stage/install"

app="$stage/Papertrail"
mkdir -p "$app/licenses"
cp "$stage/install/bin/papertrail.exe" "$stage/install/bin/papertrail-worker.exe" "$app/"
# sherpa-onnx and ONNX Runtime, for the natural voices, which need only Windows' own DLLs.
for dll in sherpa-onnx-c-api.dll onnxruntime.dll onnxruntime_providers_shared.dll; do
    [ -f "$stage/install/bin/$dll" ] || { echo "FAILED: $dll, for the natural voices, wasn't installed" >&2; exit 1; }
    cp "$stage/install/bin/$dll" "$app/"
done
cp -r "$stage/install/share/licenses/papertrail/"* "$app/licenses/"
# English, for recognising scanned text, beside the program where it looks first.
if [ -d "$stage/install/share/papertrail/tessdata" ]; then
    cp -r "$stage/install/share/papertrail/tessdata" "$app/tessdata"
fi

# Qt's libraries and plugins. The compiler's own DLLs are found below, with everything else. Reading aloud uses
# Windows' own voices, which play by themselves, and the natural voices play through Qt Multimedia's own WASAPI
# output, so Qt Multimedia's FFmpeg plugin, and FFmpeg, stay out.
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime \
    --skip-plugin-types generic,networkinformation,sqldrivers,multimedia "$app/papertrail.exe"
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime \
    --skip-plugin-types generic,networkinformation,sqldrivers,multimedia "$app/papertrail-worker.exe"
[ -f "$app/texttospeech/qtexttospeech_sapi.dll" ] || { echo "FAILED: Windows' voices can't be used" >&2; exit 1; }

# Every DLL anything here needs from MSYS2, and what those need, until nothing new turns up.
while :; do
    added=0
    while IFS= read -r dll; do
        name="$(basename "$dll")"
        if [ ! -e "$app/$name" ]; then
            cp "$dll" "$app/"
            added=1
        fi
    done < <(find "$app" \( -iname '*.exe' -o -iname '*.dll' \) -print0 | xargs -0 -n 1 ldd 2>/dev/null \
                 | awk -v prefix="$ucrt/bin/" 'index($3, prefix) == 1 {print $3}' | sort -u)
    [ "$added" = 0 ] && break
done
# Secure connections use Windows' own Schannel, through Qt's plugin for it, so OpenSSL isn't needed.
[ -e "$app/tls/qschannelbackend.dll" ] || { echo "FAILED: Qt's Schannel plugin wasn't deployed" >&2; exit 1; }

# The licence of each MSYS2 package something came from, and of the libraries built here from source.
: > "$stage/packages"
for file in "$app"/*.dll "$app"/*/*.dll; do
    [ -e "$file" ] || continue
    for origin in "$ucrt/bin/$(basename "$file")" "$ucrt/share/qt6/plugins/$(basename "$(dirname "$file")")/$(basename "$file")"; do
        [ -e "$origin" ] && pacman -Qqo "$origin" >> "$stage/packages" 2>/dev/null && break
    done
done
# A package's licence texts: those under share/licenses, or else any file named like one, but not CMake's own.
licence_files() {
    local files
    files="$(pacman -Qlq "$1" | grep -E '/share/licenses/.+[^/]$' || true)"
    [ -n "$files" ] || files="$(pacman -Qlq "$1" | grep -v '/lib/cmake/' \
        | grep -i -E '/(licen[cs]e|copying|copyright|notice)[^/]*$' || true)"
    for file in $files; do
        [ -f "$file" ] && echo "$file"
    done
    return 0
}
missing=""
for package in $(sort -u "$stage/packages"); do
    short="${package#mingw-w64-ucrt-x86_64-}"
    # Whatever the package installs as its licence, wherever it keeps it.
    found=0
    while IFS= read -r file; do
        mkdir -p "$app/licenses/$short"
        cp "$file" "$app/licenses/$short/"
        found=1
    done < <(licence_files "$package")
    # The compiler's runtime comes from GCC, under its licence and runtime exception, which the compiler holds.
    if [ "$found" = 0 ] && [ "$short" = gcc-libs ] && [ -d "$ucrt/share/licenses/gcc" ]; then
        cp -r "$ucrt/share/licenses/gcc" "$app/licenses/$short"
        found=1
    fi
    # A few packages hold none, so their texts are kept here, taken from each project's own release.
    if [ "$found" = 0 ] && [ -d "$src/packaging/windows/licenses/$short" ]; then
        cp -r "$src/packaging/windows/licenses/$short" "$app/licenses/$short"
        found=1
    fi
    [ "$found" = 1 ] || missing="$missing $short"
done
for library in libmobi qtkeychain; do
    mkdir -p "$app/licenses/$library"
    cp "$build/_deps/$library-src/"{COPYING,LICENSE}* "$app/licenses/$library/" 2>/dev/null || true
    [ -n "$(ls -A "$app/licenses/$library")" ] || missing="$missing $library"
done
if [ -n "$missing" ]; then
    echo "FAILED: no licence text found for:$missing" >&2
    exit 1
fi
# Windows' own editors want CRLF line endings, and the installer shows this one.
sed 's/$/\r/' "$app/licenses/LICENSE" > "$app/licenses/LICENSE.txt"
rm "$app/licenses/LICENSE"
for required in libmupdf qt6-base qt6-speech djvulibre libmobi qtkeychain tesseract-ocr leptonica tessdata \
    sherpa-onnx/sherpa-onnx sherpa-onnx/onnxruntime sherpa-onnx/espeak-ng; do
    [ -d "$app/licenses/$required" ] || { echo "FAILED: the licence of $required is missing" >&2; exit 1; }
done

# The program must work with only Windows' own folders to find DLLs in.
report="$stage/self-test.txt"
if ! PATH="/c/Windows/System32:/c/Windows" "$app/papertrail.exe" --self-test --report "$(cygpath -w "$report")"; then
    cat "$report" >&2 || true
    echo "FAILED: the packaged program's self-test" >&2
    exit 1
fi
cat "$report"

# The portable zip: the same files, and the marker that keeps its library beside it.
portable="$stage/Papertrail-$version-portable"
cp -r "$app" "$portable"
printf '%s\r\n' \
    "This copy of Papertrail is portable: it keeps its library in the \"Papertrail library\" folder beside it," \
    "so the whole folder can be carried on a USB stick. Delete this file to use the standard folders instead." \
    > "$portable/portable.txt"
rm -f "$out/Papertrail-$version-portable.zip"
(cd "$stage" && bsdtar -a -cf "$out/Papertrail-$version-portable.zip" "Papertrail-$version-portable")

# The installer, which installs and removes exactly the files listed here.
files="$stage/files.nsh"
unfiles="$stage/unfiles.nsh"
: > "$files"
: > "$unfiles"
(cd "$app" && find . -type d | sort) | while IFS= read -r dir; do
    rel="${dir#./}"
    [ "$dir" = "." ] && rel=""
    target='$INSTDIR'${rel:+\\${rel//\//\\}}
    echo "SetOutPath \"$target\"" >> "$files"
    (cd "$app/$dir" && find . -maxdepth 1 -type f | sort) | while IFS= read -r file; do
        name="${file#./}"
        echo "File \"$(cygpath -w "$app/$dir/$name")\"" >> "$files"
        echo "Delete \"$target\\$name\"" >> "$unfiles"
    done
done
(cd "$app" && find . -mindepth 1 -type d | sort -r) | while IFS= read -r dir; do
    rel="${dir#./}"
    echo "RMDir \"\$INSTDIR\\${rel//\//\\}\"" >> "$unfiles"
done
echo 'SetOutPath "$INSTDIR"' >> "$files"
size_kb="$(du -sk "$app" | cut -f1)"
cp "$src/packaging/windows/papertrail.ico" "$stage/"
cp "$src/packaging/windows/papertrail.nsi" "$stage/"
makensis -V2 -INPUTCHARSET UTF8 \
    -DVERSION="$version" \
    -DSOURCE="$(cygpath -w "$app")" \
    -DOUTFILE="$(cygpath -w "$out/Papertrail-$version-setup.exe")" \
    -DFILES="$(cygpath -w "$files")" \
    -DUNFILES="$(cygpath -w "$unfiles")" \
    -DSIZE_KB="$size_kb" \
    "$(cygpath -w "$stage/papertrail.nsi")"

echo "Built:"
ls -l "$out/Papertrail-$version-setup.exe" "$out/Papertrail-$version-portable.zip"
