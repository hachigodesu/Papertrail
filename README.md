# Papertrail

[![Windows](https://github.com/hachigodesu/Papertrail/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/windows.yml)
[![Arch](https://github.com/hachigodesu/Papertrail/actions/workflows/arch.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/arch.yml)
[![Ubuntu](https://github.com/hachigodesu/Papertrail/actions/workflows/ubuntu.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/ubuntu.yml)
[![AppImage](https://github.com/hachigodesu/Papertrail/actions/workflows/appimage.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/appimage.yml)
[![Flatpak](https://github.com/hachigodesu/Papertrail/actions/workflows/flatpak.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/flatpak.yml)

A desktop library manager and reader for university students and researchers, for Windows and Linux. Papertrail indexes your journal articles, textbooks, theses, reports and standards where they already are, and never changes your files.

- **Browse and find:** a cover grid and a detailed list, tabs for each kind of publication, filters with counts, and a search box that understands fields such as `author:`, `year:>2010` and `tag:`. It stays quick with 100,000 publications.
- **Read:** PDF, EPUB, DjVu, MOBI, AZW3, XPS, CBZ and FB2 in the built-in reader, with contents, thumbnails, search, two-page view, and light, dark and sepia pages. It opens where you left off.
- **Highlight and take notes,** which stay in Papertrail and never change your files. Search them, list them all, or save a copy of a PDF with them in it.
- **Organise:** tags within tags, collections within collections, and journal issues split into their articles, even without a table of contents.
- **Search inside your documents,** and open each match at its page.
- **Fill in details** from Crossref, OpenAlex, arXiv and Open Library, if you choose to, with every change reviewable and undoable. It also warns you of retractions.
- **Cite:** copy references in Harvard, APA, IEEE or Chicago style, and export BibTeX, RIS or CSL-JSON.
- **Move to another computer** with your tags, notes and settings, even if your files are in different folders there.
- **Summaries from Claude** with your own API key, if you choose to, straight away or at half price in batches.
- **Backups** of your library every day, and before anything that changes a lot of it.

## Installing

Download from the [releases page](https://github.com/hachigodesu/Papertrail/releases).

### Windows 10 and 11

- **The installer** (`Papertrail-<version>-setup.exe`) asks whether to install just for you, which needs no administrator rights, or for everyone on the computer.
- **The portable zip** (`Papertrail-<version>-portable.zip`) runs from any folder, such as a USB stick, and keeps its library in a `Papertrail library` folder beside it. Unzip it and open `papertrail.exe`.

The downloads aren't signed with a code-signing certificate, so the first time you open one, Windows may say "Windows protected your PC". Choose **More info**, then **Run anyway**. Each release's `SHA256SUMS` file lets you check a download is the one published.

### Linux

- **AppImage** (`Papertrail-<version>-x86_64.AppImage`), for most distros from Ubuntu 22.04, Debian 12 and Fedora on. Make it executable and open it:

  ```bash
  chmod +x Papertrail-*-x86_64.AppImage
  ```

- **Flatpak** (`Papertrail-<version>.flatpak`), for any distro with Flatpak, including immutable ones such as Fedora Silverblue. It installs the KDE runtime it needs from Flathub:

  ```bash
  flatpak install --user Papertrail-*.flatpak
  ```

  The Flatpak can read all your files, so it can index them wherever they are, but not change them. Exports and copies with highlights are saved through your desktop's file chooser.

- **Arch:** `packaging/arch/PKGBUILD` builds a native package with `makepkg -si`.

## Getting started

When Papertrail first opens, add a folder of papers and books, or bring in a library exported from Papertrail on another computer. Papertrail asks whether to look up details online, and you can change that later under **File > Online lookups...**.

Your library is kept in your user folder: `%APPDATA%\Papertrail` on Windows, and `~/.local/share/Papertrail` on Linux (`~/.var/app/io.github.hachigodesu.Papertrail` for the Flatpak). Backups are in a `backups` folder beside it. Uninstalling Papertrail leaves your library where it is.

## Your files and your privacy

- **Your documents are never changed.** Highlights, notes and details are kept in Papertrail's own library.
- **Papertrail stays offline until you choose otherwise.** If you turn lookups on, it sends a document's DOI, ISBN, arXiv number or title to Crossref, OpenAlex, arXiv or Open Library, and DOIs to Crossref to check for retractions. It never sends your files, notes or tags. On Linux, lookups need OpenSSL 3.
- **Summaries are sent to Anthropic only when you ask,** using your own API key and paid for by you. You're shown what will be sent and an estimated cost first. It sends identifying details such as the DOI and title, and Claude looks for the paper online. Your file is only sent if you choose that, with a confirmation of its own. The key is kept in your system's credential store, never in the library or its exports.
- **There's no telemetry** and nothing is sent anywhere else.

## Building from source

Requires CMake 3.25+, Ninja, a C++20 compiler and Qt 6.12 or newer. Windows builds must stay on Qt 6.12, the last release to support Windows 10.

### Windows (MSYS2 UCRT64)

Install MSYS2 at `C:\msys64`, then in a UCRT64 shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-libmupdf mingw-w64-ucrt-x86_64-djvulibre mingw-w64-ucrt-x86_64-zlib
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

`packaging/windows/build-package.sh` makes the installer and portable zip, with NSIS installed (`mingw-w64-ucrt-x86_64-nsis`).

### Linux

Install Qt 6.12 or newer, MuPDF, DjVuLibre, libsecret (so that an API key can be kept in the desktop's keyring), CMake and Ninja, then use the same commands with the `linux-debug` preset.

On Arch:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base libmupdf djvulibre libsecret zlib
```

Ubuntu's own Qt and MuPDF packages are too old. Install Qt with [aqtinstall](https://github.com/miurahr/aqtinstall) and pass its location to CMake with `-DCMAKE_PREFIX_PATH`, and build MuPDF with `packaging/linux/install-mupdf.sh`.

`packaging/linux/build-appimage.sh` makes the AppImage, and `packaging/flatpak/` holds the Flatpak's manifest.

### Checking an install

`papertrail --self-test` checks a copy can read each kind of document and make a library, without opening a window or your own library, and exits with 0 if everything works. `papertrail --library FOLDER` opens a library kept in another folder.

## Reporting problems

Report bugs on the [issue tracker](https://github.com/hachigodesu/Papertrail/issues). See [SECURITY.md](SECURITY.md) for security problems. Please don't attach documents you don't have the right to share.

## Licence

Papertrail is free software under the GNU Affero General Public License v3.0 or later (see `LICENSE`). It includes Qt, MuPDF, DjVuLibre, libmobi, SQLite, QtKeychain, zlib and OpenSSL, among others: see [packaging/THIRD-PARTY.md](packaging/THIRD-PARTY.md).
