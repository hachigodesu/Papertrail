# M3 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC.

M3 makes Papertrail a usable library window: you can add folders, browse in a grid or list, filter, search, tag, and edit metadata. Documents still open in the system's own app until the reader arrives in M4.

## How it's been tested

- **Automated:** all 10 test programs pass on all three platforms, including 38 new tests of the window, most of which drive the real thing with no screen attached. They use real sample documents and the real worker, and check that imported files stay unchanged.
- **By eye:** pictures of the window were checked on Windows, in light and dark mode. The app itself was also started on all three platforms.
- **Not tested yet:**
  - Using it by hand on a real collection, which is M3's goal. Clicking and dragging with a real mouse hasn't been tried either.
  - The window on a real Linux desktop, under Wayland or X11. Linux has only run with no screen attached.

## What's in the window

- **Tabs:** All, Journal Volumes, Journal Articles, Preprints, Conference Papers, Textbooks, Theses & Dissertations, Technical Reports, Standards, Other.
- **Grid and list:** both show the front page, title, authors and year, and share one selection.
- **Search box:** plain words, `"exact phrases"`, and field searches such as `author:`, `year:>2010`, `tag:` and `doi:`. A search shows the best matches first until another sort is picked.
- **Filter sidebar:** tags (as a tree), year range, author, journal, publisher, conference, institution, language, format, read status, and five "needs attention" flags, all with live counts.
- **Details panel:** cover, all metadata, read status, rating, notes, tags, and file locations. DOI, arXiv and web links open in the browser.
- **Metadata editor:** offers the fields that suit the publication's type, plus any other field that already has a value. Each change keeps the earlier value, ready for undo in M6.
- **Tags:** added from the details panel or to a whole selection, with `Engineering > FEM` creating both levels. They can be renamed, coloured, moved and deleted from the sidebar.
- **Changes to many at once:** tag, set read status, or mark as checked.
- **Import:** Add folder, a Library folders dialog (scan again, change location, remove), progress in the status bar, and a Stop button.

## Performance at 100,000 publications

These are from `test_ui_perf`, using release builds and real cover files, with no screen attached.

| | Target | Windows | Arch | Ubuntu |
|---|---|---|---|---|
| Window showing its first rows | 2,000 ms | 67 ms | 63 ms | 64 ms |
| Change tab | 100 ms | 7 ms | 10 ms | 11 ms |
| Search for a field | 100 ms | 10 ms | 14 ms | 15 ms |
| Search for a common word, best match first | 250 ms | 61 ms | 55 ms | 59 ms |
| Sidebar counts, in the background | none | 308 ms | 270 ms | 251 ms |
| Grid: wheel scrolling, 95% of frames within | 16.7 ms | 5.3 ms | 10.6 ms | 12.1 ms |
| Grid: dragging the scroll bar, 95% of frames within | 16.7 ms | 9.3 ms | 9.8 ms | 10.4 ms |
| Grid: resizing the window, 95% of frames within | none | 16.6 ms | 24.8 ms | 24.5 ms |
| List: wheel scrolling, 95% of frames within | 16.7 ms | 5.9 ms | 8.0 ms | 8.1 ms |
| List: dragging the scroll bar, 95% of frames within | 16.7 ms | 2.6 ms | 9.4 ms | 7.9 ms |
| Tag all 100,000: back to the window | 200 ms | 8 ms | 10 ms | 10 ms |
| Tag all 100,000: finished, in the background | none | 23 s | 17 s | 16 s |
| Longest the window paused during that | none | 15 ms | 2 ms | 4 ms |

These measure Papertrail's own drawing with Qt's software painting, on a window of 1,400 by 900. They don't include a real graphics driver or desktop compositor, so they don't prove smoothness on a real screen. Resizing the window is a little slower than a 60 Hz frame on Linux, because the grid rearranges every row.

## What testing changed

- **Tagging everything froze the window for 10 seconds,** because each publication updates the search index. Changes to many publications now run in the background in batches. This takes longer overall, but the window stays free.
- **Grid scrolling on Ubuntu was close to the limit,** at 16.4 ms. Covers are now held at the size they're shown, which brought it down to 12.1 ms.
- **Dark mode hid the filter checkboxes and arrows, and clipped the details panel's lists.** All are fixed. The sidebar now draws its own checkboxes and arrows from the theme's text colour.
- **A selection made in the grid only highlighted the title when switching to the list,** and Windows' dark theme made alternate list rows look selected. Whole rows are now selected, and rows no longer alternate in colour.
- **Watching folders stopped people renaming them on Windows.** A test showed that a parent folder couldn't be renamed while Papertrail was watching it, because Windows' folder watching keeps each folder open. See below.
- **A missing worker would have marked every file unreadable,** and that would have stuck. The import now checks first and reports the problem.
- **Metadata could have been treated as rich text or as a link to anything.** Text from documents is now always shown as plain text, and only `http` and `https` links can be followed, so a `file:` or custom-scheme link in a document's metadata is shown but can't be opened.

## Changes from the plan

- **Preprints have their own tab,** and only ever hold what you've marked yourself, with the Preprint checkbox above the notes. Papertrail doesn't sort anything there by itself. Automatic sorting from arXiv numbers, DOIs and first-page wording was tried, but it put too many published papers there without flagging them, because they carry much of the same wording. Libraries that went through it have those results undone when they next open. M6 may be able to suggest preprints using Crossref's records, which would be for you to accept.
- **Watched folders work differently on each system.**
  - **Linux** is told about changes as they happen, for up to 5,000 folders. Beyond that, or if it runs out of system watches, it falls back to the approach below.
  - **Windows** can't do this without locking folders, so it scans every five minutes, and when you come back to the window, at most once a minute. Slow disks are scanned less often. This means a new file can take up to five minutes to appear if you don't switch to Papertrail.
  - The setting is under File, and a scan at startup remains the safety net.
- **Files that are still being copied are left for a later scan,** to stop half-copied files being read as damaged.
- **A library folder inside another looks after its own files,** so nothing appears as its own duplicate. Adding a folder that's already inside the library just scans it again.
- **Only one copy of Papertrail can open a library at a time,** because two would each restart the other's import jobs.
- **Window size and layout aren't exported.** They're stored as settings that belong to one computer.

## Known limits

- **Documents open in the system's app** until M4.
- **Filter lists show the 50 most common values,** with a note saying so. Others can be reached with the search box, such as `author:`.
- **A selection of many publications is cleared if the number of results changes,** such as during an import, so a change can't land on the wrong publications.
- **Journal names and 41% of authors are still missing** from imported files until online lookup in M6, so those filters are thin on real collections.
- **Bulk re-run lookup and citation export** arrive with M6 and M10.
- **Removing publications** isn't offered. It isn't in the plan, and a publication would come back only if its file changed.

## How to try it

On Windows, double-click `tools/windows/run-debug.cmd`. Opening `papertrail.exe` directly fails with a missing `Qt6Gui.dll`, because the build relies on MSYS2's DLLs until there's a proper Windows package in M12.

Your library is stored in your user's app data folder, and is empty until you add a folder. Your documents are only read, never changed.
