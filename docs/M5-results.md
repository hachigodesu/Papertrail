# M5 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC.

All the planned formats now open in the reader, with a cover and page count, and with text where the file has any.

| Format | Reader | Cover | Text | Contents | Links | Tested with |
|---|---|---|---|---|---|---|
| PDF | Yes | Yes | Yes | Yes | Yes | Generated samples (M4), and your papers by hand |
| EPUB | Yes | Yes | Yes | Yes | Yes | Generated samples, EPUB 2 and 3 |
| AZW3 (KF8) | Yes, through a converted copy | Yes | Yes | Yes | Depends on the book | One real book |
| Joint MOBI and KF8 | Yes, using the KF8 part | Yes | Yes | Yes | Depends on the book | A file built from samples |
| Older MOBI | Yes | Yes | Yes | None in the sample | None in the sample | A tiny generated sample |
| FB2 | Yes | Yes | Yes | None in the sample | None in the sample | A tiny generated sample |
| DjVu | Yes | Yes | Yes, if it has a text layer | Yes | Yes | Generated samples |
| XPS | Yes | Yes | Yes | None in the sample | None in the sample | A generated sample, and one real file made by Windows |
| CBZ | Yes | Yes | Has none | Has none | Has none | A tiny generated sample |

## What changed

### AZW3

MuPDF opens these without complaint and then shows a single blank page, so they have to be converted first. The worker now converts them to EPUB using libmobi, and keeps the result in the cache folder, named after the file's contents. Your original file is only read.

- **The first time a book opens** takes a moment longer. After that, the converted copy is reused.
- **A book locked with DRM** is listed and says so, and neither the import nor the reader tries to read it. Papertrail doesn't remove DRM.
- **A file that can't be converted** is still listed with whatever its header says, and is marked as unreadable with the reason.
- **Books already in your library** get their covers and page counts the next time it opens. Their details aren't changed.

libmobi is built from pinned source on every platform, and only its library, so it adds no zlib or libxml2 dependency to install.

### DjVu

DjVu now has what the other formats have: search, including phrases across a line break, text selection, a table of contents, and links. These only work where the file has a hidden text layer, so scanned books without one can't be searched until OCR arrives.

### Other names for formats

`.azw` and `.oxps` now open. MuPDF reads those formats, but didn't recognise the names.

## Findings left open from M0

- **Joint Kindle files work,** in that a file holding both old and new sections opens as its KF8 part. No real one has been tried. There was none to hand, so the test builds one from the samples.
- **XPS text works.** M0 couldn't test this, because the sample has no embedded font. A 9-page file made by Windows' own document classes could be searched and selected. That file wasn't committed, because it embeds a subset of a Windows font.

## How it's been tested

- **Automated:** all 11 test programs pass on all three platforms. Every format goes through both the import and the reader, and the reader's first page must have something visible on it.
- **Crash isolation still holds.** On Arch, `tests/samples/truncated.pdf` still crashes the reader process (exit status 134), and the window tests pass.
- **By eye:** pictures of the real KF8 book, in the library and the reader, were checked on Windows.
- **Not tested yet:**
  - Real files in most of these formats. Only one real KF8 book and one real XPS file have been tried, and the MOBI, FB2, CBZ and DjVu samples are tiny and generated. Real books will be messier.
  - A book that's really locked with DRM. The test marks an ordinary book as locked in its header.
  - Timing on large files in these formats. M4's timings were for PDF and EPUB only.
  - The other formats by hand, or on a real Linux desktop.

## Sample file

`tests/samples/kf8_frankenstein.azw3` is a real book, *Frankenstein* from Project Gutenberg, because nothing here can write KF8. It adds 548 KB to the repository, and it will become public along with the repository at V1. The book is in the public domain in the United States, and the file includes the Project Gutenberg License. See `tests/samples/README.md`.

## Known limits

- **Dark mode inverts images,** which matters more for CBZ comics and image-heavy DjVu scans than for text. This is unchanged from M4.
- **A DjVu link only works if it's a rectangle, oval or text area.** Other shapes are skipped.
- **Converted copies stay in the cache folder.** Nothing cleans them up yet, though they're only about the size of the book, and they can safely be deleted.
- **A saved place in AZW3 belongs to the converted copy's layout,** the same as an EPUB.
- **MuPDF and libmobi are still reading untrusted files.** libmobi's last repository push was in October 2024, so any fixes may be ours to make. It only runs in the worker, so a crash there is contained.
