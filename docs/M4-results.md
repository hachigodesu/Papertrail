# M4 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC.

Double-clicking a publication now opens it in a tab inside Papertrail. M4 was planned for PDF and EPUB, and those are what have been tested. Other formats go through the same reader, and M5 will test them.

## What's in the reader

- **Tabs:** each document opens in its own tab, and the library is always the first. The tab bar only appears once a document is open.
- **Layouts:** continuous scrolling, single page or two pages, and these are remembered as your default.
- **Zoom:** fit width, fit page, or a set percentage, with 100% being actual size. Ctrl+wheel, + and - work too.
- **Contents and pages:** a side panel with the table of contents and page thumbnails. Thumbnails only load as they come into view.
- **Search within the document:** matches appear as they're found, starting from where you are, and you can step through them.
- **Selecting text:** drag to select, including across pages, then Ctrl+C to copy.
- **Page colours:** light, dark or sepia.
- **EPUB text size:** larger or smaller text, which lays the book out again and keeps your place.
- **Links:** links within a document go to their page, and Back returns you. Web links open in your browser. Anything else, such as a `file:` link, isn't followed.
- **Coming back:** each document opens where you left it, at the same zoom, including after restarting.

## One publication in several formats

Trying the reader showed that a book held as both a PDF and an EPUB appeared as two publications. It's now one, with both files.

**Choosing a format.** Double-clicking a publication with more than one format asks which to open, and the details panel has a button for each. Each format has its own tab and its own reading place, because page 40 of a PDF and page 40 of an EPUB are different places.

**How they're recognised.** Different formats are joined if any of these is true, in order of confidence:

| Evidence | Result |
|---|---|
| The same file name in the same folder, apart from the extension | Joined |
| The same DOI or ISBN, and a title that agrees | Joined |
| The same title, first author and year, with a title long enough to be distinctive | Joined, and marked as needing a check |

Two files in the same format are never joined this way, because they may be different editions. They still merge if they're byte-for-byte identical, as they always have.

An ebook and a print PDF often have different ISBNs and differently worded titles. If they also have different file names, Papertrail won't recognise them, so they can be joined by hand: select both, right-click, and choose **Combine into one publication**.

**If it gets one wrong,** the details panel has a button to separate that format into a publication of its own, with a copy of the details and tags. The new one is marked as needing a check.

**Combining** keeps the first publication's details and fills in only what it's missing. Tags, notes, highlights and reading places all carry over. If the second file's title is real and the first's was only a guess, the real one is used, unless you've set a title by hand.

**Existing libraries** are looked through once, the next time they open, so books imported as two publications get joined. The status bar says how many.

## The main decision: each document gets its own process

M2 found that Arch's MuPDF crashes outright on a damaged PDF. The plan had the reader opening files inside the app, where that crash would take the whole window with it. Refusing files that failed during import wouldn't have been enough, because import only reads the first few pages, and a bad page could be anywhere.

So each open document runs in its own copy of `papertrail-worker`, which sends finished pages back to the window.

| If a document... | What happens |
|---|---|
| Crashes while opening | The tab says the file may be damaged, and offers to open it in another app |
| Crashes on one page | That page says it couldn't be shown. The process restarts by itself, and the rest of the document carries on |
| Hangs on one page | The same, after 60 seconds |
| Keeps crashing | After 10 restarts, the tab says so and stops trying |
| Needs a password | The tab says Papertrail can't open those yet, and offers another app |

This isn't only theoretical. On Arch, opening `tests/samples/truncated.pdf` still crashes the reader process with a double free (exit status 134), and the window carries on unaffected.

The extra process turned out to cost very little: at 1,190 by 1,684 pixels, a page reaches the window in about 15 to 25 ms, including rendering and sending.

## Performance

These are from `test_reader_perf`, on a generated 3,000-page, 17 MB book with text and simple drawings on every page, using release builds with no screen attached.

| | Target | Windows | Arch | Ubuntu |
|---|---|---|---|---|
| Opened, with pages counted | 2,000 ms | 77 ms | 183 ms | 162 ms |
| First page ready | 500 ms | 123 ms | 231 ms | 212 ms |
| Every page measured, in the background | none | 244 ms | 332 ms | 312 ms |
| A random page at 893 by 1,263 pixels (median) | 200 ms | 8.9 ms | 13.2 ms | 13.2 ms |
| A random page at 1,190 by 1,684 pixels (median) | 200 ms | 15.1 ms | 24.2 ms | 24.5 ms |
| A random page at 1,785 by 2,526 pixels (median) | none | 33.5 ms | 66.2 ms | 64.0 ms |
| Searching every page | none | 3.9 s | 3.6 s | 4.2 s |
| Scrolling, 95% of frames within | 16.7 ms | 2.1 ms | 4.5 ms | 4.3 ms |
| Jumping to a far page, ready | none | 118 ms | 207 ms | 207 ms |

The search and page-turning timings run while the window stays free, because the work happens in the other process.

### Harder files

The generated book is easy to draw, so two harder kinds of file were timed once on Windows, against the reader process directly.

| File | Result |
|---|---|
| A scanned PDF: 60 pages of 300 dpi images, with some noise in them | About 35 ms a page at 1,190 by 1,684 pixels |
| An extremely long EPUB: 10,000 pages | 3.9 s to open, and 7.8 s to change the text size. The window stays usable during both, and pages then appear in under 15 ms |

An EPUB has to be laid out in full before its page count is known, so opening takes time in proportion to the book's length. A typical book of a few hundred pages should take a fraction of a second, though that's scaled from this result and hasn't been measured itself.

Real textbooks, with complex figures and many fonts, haven't been timed. They'll be slower than the generated book, and how much so is unknown.

## How it's been tested

- **Automated:** all 11 test programs pass on all three platforms. That includes 44 new reader tests using the real document process and real sample files, including ones built to crash or hang on a chosen page.
- **Files stay untouched:** reading, searching and closing a document leaves its contents and modified date unchanged.
- **By eye:** pictures of the reader were checked on Windows, in each page colour and layout.
- **Not tested yet:**
  - Using it by hand on your own papers, with a real mouse and scroll wheel.
  - The reader on a real Linux desktop.
  - A display scaled above 100%. The code renders for high-resolution screens, but none has been tried.

## What testing changed

- **A place in an EPUB didn't survive a restart.** MuPDF's bookmarks only mean anything inside one process. Places are now stored as a chapter and how far through it, which also survives a change of text size. Within a session, changing the text size still uses MuPDF's bookmarks, which are more exact.
- **One landscape page pushed every other page sideways,** and added a scroll bar to the whole document. Pages are now centred on their own, a typical page sets the fit-width size, and the scroll bar only appears while a wider page is on screen.
- **An EPUB 3 table of contents was a suspected gap from M2.** MuPDF reads them after all, including nested entries, and it stops before the landmarks list. Both kinds of table of contents needed their entries resolved to pages, because MuPDF leaves those out for EPUBs.

## Changes from the plan

- **Documents open in separate processes** rather than inside the app, as above.
- **Font size is one setting across all EPUBs,** rather than per book.
- **Dark mode inverts the page, images included.** It's applied as the page is drawn, so changing it is instant, but photographs will look like negatives. A gentler treatment of images could come later.
- **Password-protected files can't be opened yet,** and the tab says so. There's no password prompt, because the plan doesn't include one.

## Known limits

- **Separating formats isn't remembered if a folder is removed and added back.** That counts as a new import, so the formats would be joined again.
- **Thumbnails, search and selection on other formats** are for M5 to check. DjVu has no search or selection through this yet.
- **AZW3 can't be opened yet.** It needs the conversion planned for M5, and the tab offers another app.
- **Highlights and notes on pages** arrive in M9.
- **Selection is limited to 30 pages at a time.**
- **Search stops at 512 matches on any one page,** and it doesn't search across a page break.
- **In an EPUB, a saved place is approximate after a text size change:** the same chapter, the same share of the way through it.

## How to try it

On Windows, double-click `tools/windows/run-debug.cmd`, then double-click a publication.
