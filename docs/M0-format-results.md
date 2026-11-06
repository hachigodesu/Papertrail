# M0 results

Tested on 6 November 2026 in `tools/format_probe` and with real runs of the app.

## Platforms

| Platform | Compiler | Qt | MuPDF | DjVuLibre | Build and tests | App starts |
|---|---|---|---|---|---|---|
| Windows 10 (MSYS2 UCRT64) | GCC 16.2 | 6.12.0 | 1.28.5 | 3.5.30 | Pass | Real window confirmed by title |
| Arch (WSL2) | GCC 16.2.1 | 6.12.0 | 1.28.5 | 3.5.30.1 | Pass | Wayland and X11 |
| Ubuntu 24.04 (WSL2) | GCC 13.3 | 6.12.0 (aqtinstall) | 1.28.5 (source) | 3.5.28 | Pass | Wayland and X11 |

On Linux, "app starts" means it kept running for 6 seconds without errors. Nobody has looked at the window itself. Only the placeholder window and worker stub exist so far.

## Formats

All three platforms gave identical results. The test files are tiny ones from `tools/format_probe/make_samples.py`, so they show each format's basic path works, not how real-world files behave.

| Format | Opens | Renders | Text returned | Engine |
|---|---|---|---|---|
| PDF | Yes | Yes | Yes | MuPDF |
| EPUB | Yes | Yes | Yes | MuPDF |
| FB2 | Yes | Yes | Yes | MuPDF |
| MOBI (older PalmDOC type) | Yes | Yes | Yes | MuPDF |
| CBZ | Yes | Yes | Has no text | MuPDF |
| XPS | Yes | Yes | Sample has no text | MuPDF |
| DjVu | Yes | Yes | Not probed (sample has no text layer) | DjVuLibre |
| AZW3 (KF8), in MuPDF directly | Yes | **Blank page** | **None** | **MuPDF can't read it** |
| AZW3 (KF8), converted to EPUB | Yes | Yes | Yes | libmobi v0.12, then MuPDF |

### Format findings

- **MuPDF handles MOBI and FB2**, even though its website doesn't list them. libmobi isn't needed for those.
- **MuPDF can't read AZW3, and it fails silently.**
  - Tested with a real book: Project Gutenberg's *Frankenstein* in KF8 format (pure KF8, MOBI version 8), which isn't committed to this repo.
  - It opens with no error, but gives one blank page and no text, plus `cannot find entry kindle:flow:…` warnings.
  - The engine layer therefore has to spot KF8 itself and never pass it to MuPDF, and "it opened" can't count as success on import.
- **libmobi fixes this, so AZW3 stays in V1.**
  - libmobi v0.12's `mobitool -e` converted the book to EPUB on all three platforms, with the same 421,394-byte size each time.
  - MuPDF then read 99 pages with the text intact.
  - That's one book, made by calibre, so other AZW3 files may behave differently. Joint MOBI and KF8 files are still untested.
- **format-probe reports "ok" for that blank result**, so its output needs reading alongside `ink` and `text`.

### How AZW3 will work (M5)

1. Detect KF8 from the file's contents: `BOOKMOBI` with MOBI version 8, or EXTH record 121 for joint files.
2. Convert it to EPUB in the worker process, because this parses untrusted files, and keep the EPUB in the cache folder, named by the file's content hash. The original file is never touched.
3. Open the cached EPUB with MuPDF. Annotations are stored against the original file's hash.

libmobi details:

- **Licence:** LGPL-3.0, compatible with Papertrail's AGPL.
- **Version:** pinned to v0.12, built from source on every platform because MSYS2 doesn't package it. Its last repository push was in October 2024, so we may need to maintain fixes ourselves.
- **Build options:** `USE_LIBXML2=OFF`, `USE_ENCRYPTION=OFF` (Papertrail doesn't handle DRM) and `BUILD_SHARED_LIBS=OFF`. With encryption off, the `mobidrm` tool doesn't link, so build only what's needed.
- **Dependency:** zlib.
- **Not yet done:** this used the `mobitool` command. M5 still has to call the library directly.
- **EPUB 3:** MuPDF prints `unknown epub version: 3.0` but still renders the file. M4 must check EPUB 3 tables of contents and real books.
- **XPS text is untested**, because text in XPS needs an embedded font. Test with a real file in M5.

## AppImage

Built with `packaging/linux/build-appimage.sh` on Ubuntu 24.04. It's 30 MB and bundles Qt, but not yet MuPDF or DjVuLibre, because the app doesn't link them yet.

| Tested on | Result |
|---|---|
| Ubuntu 24.04, no `libfuse2` installed | Runs directly, on Wayland and X11 |
| Arch, minimal install | Fails to mount: no `fusermount` found |
| Arch, with `APPIMAGE_EXTRACT_AND_RUN=1` | Runs on Wayland and X11, using the bundled Qt rather than Arch's |

### AppImage findings

- **Glibc baseline: 2.35, by building releases on Ubuntu 22.04.**
  - Qt 6.12's own binaries need glibc 2.34.
  - This trial was built on Ubuntu 24.04, so its contents need 2.38, which is too new for a release.
  - Ubuntu 22.04 is on Qt's supported list and has glibc 2.35. Older distros use the Flatpak.
- **Linux systems with no FUSE tools can't mount the AppImage.** Those users need their distro's FUSE package (untested) or the extract-and-run fallback. This must be in the install docs.

## Not done yet

- A release-baseline AppImage build on Ubuntu 22.04.
- AppImage checks on Debian, Fedora and openSUSE, which belong in CI.
- The format probe in CI: CI builds and tests the app on Windows, Arch and Ubuntu, and all three pass, but doesn't build the probe yet.
