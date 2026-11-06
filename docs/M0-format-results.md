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
| AZW3 (KF8) | **Untested** | **Untested** | **Untested** | Needs a real file |

### Format findings

- **MuPDF handles MOBI and FB2**, even though its website doesn't list them. libmobi isn't needed for those.
- **AZW3 is still open.** The library contains `azw` but no `azw3` or `kf8` strings, and a valid KF8 file can't be written by hand. libmobi remains the fallback.
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
- CI itself has never run.
- AZW3.
