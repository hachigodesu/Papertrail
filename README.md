# Papertrail

[![Windows](https://github.com/hachigodesu/Papertrail/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/windows.yml)
[![Arch](https://github.com/hachigodesu/Papertrail/actions/workflows/arch.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/arch.yml)
[![Ubuntu](https://github.com/hachigodesu/Papertrail/actions/workflows/ubuntu.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/ubuntu.yml)
[![AppImage](https://github.com/hachigodesu/Papertrail/actions/workflows/appimage.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/appimage.yml)

A desktop library manager and reader for university students and researchers, for Windows and Linux. Papertrail indexes your journal articles, textbooks, theses, reports and standards where they already are, and never modifies your files.

**Status:** early development. You can browse, filter, tag and edit your library, and read PDF and EPUB files in the built-in reader. See [docs/PLAN.md](docs/PLAN.md) for the full plan.

## Building

Requires CMake 3.25+, Ninja, a C++20 compiler and Qt 6.12 or newer. Windows builds must stay on Qt 6.12, the last release to support Windows 10.

### Windows (MSYS2 UCRT64)

Install MSYS2 at `C:\msys64`, then in a UCRT64 shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-libmupdf mingw-w64-ucrt-x86_64-djvulibre
```

Then from the project folder:

```bash
cmake --preset windows-debug
```

```bash
cmake --build --preset windows-debug
```

```bash
ctest --preset windows-debug
```

### Linux

Install Qt 6.12 or newer, MuPDF, DjVuLibre, CMake and Ninja, then use the same commands with the `linux-debug` preset.

On Arch:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base libmupdf djvulibre
```

Ubuntu's own Qt and MuPDF packages are too old. Install Qt with [aqtinstall](https://github.com/miurahr/aqtinstall) and pass its location to CMake with `-DCMAKE_PREFIX_PATH`, and build MuPDF with `packaging/linux/install-mupdf.sh`.

## Licence

GNU Affero General Public License v3.0 or later.
