# M11 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC. Built while you were out, so nothing has been tried by hand yet.

You can now move a library to another computer, with its files in a completely different folder structure, and get back its details, tags, collections, highlights, notes, summaries, places and settings.

## How it works

- **Exporting:** **File > Export library...** first says what goes in the file and what doesn't, then saves one `.papertrail` file.
- **Importing:** **File > Import library...** says what's in the file, and lets you choose which parts to bring in and whether to add to the library or replace it. **Library folders...** is there too, since files are found in the library's folders.
- **Finding files:** an export describes each file only by its contents, size, format and name, never by where it was. On import:
  1. **Same contents:** connected straight away, wherever the file is and whatever it's called.
  2. **Same DOI, ISBN or arXiv number, but other contents,** such as another download of the same paper: suggested for you to check.
  3. **Same name and size, but other contents:** suggested too.
  4. **Nothing found:** the publication is kept, with all its details, out of sight. When a file with its contents turns up in a later scan, it's connected and appears by itself.
- **Afterwards:** a report gives the counts, then the library is scanned so waiting publications can find their files. **Check suggested files...** (also under **File**) lists the suggestions to accept or reject. Accepting one that's already another publication's makes the two one, keeping the imported details, and marks highlights from the other computer's copy as needing checking, since the pages may not line up.

## Changed after hand testing

- **Imported publications stay out of sight until their files are found.** They're hidden as those kept for a removed folder are, and each appears by itself when its file turns up in a scan, when a suggested file is accepted, or when one's found by hand. A library that imported before this hides its waiting publications when it's next opened. They're still exported, with the fingerprints of the files they're waiting for.
- **Library folders** says how many are waiting, with **Delete them...** to stop waiting. Those waiting aren't counted among the publications kept for removed folders.
- **Several library folders can be chosen at once** in **Library folders**, to scan again or remove together with one question. A publication in two of the removed folders counts as leaving, not staying. **Change location...** still takes one folder at a time.

## Other fixes from hand testing

- **Dragging across the grid** chooses every cover in the rows it crosses, between the columns it touches, as far as the last cover, and shows the area being dragged. Qt's list mode only follows a drag while the pointer is over a cover unless its selection rectangle is shown, so a drag ending in the empty space beside a short last row stopped at the last cover passed over. `GridView` also works out the covers itself, since Qt's own choice left out a short last row. The test sends real mouse events, after the first fix passed a test that skipped them.
- **Issue titles.** An issue made from a file took the largest text on its cover as its title, which was often "Publication Information" or a line of numbers and dates. `import/JournalIssues` now notices a title like that, reads any volume, issue, date and ISSN from it, takes the journal from an article that knows it, and makes the title from those details. Articles that had taken the cover's words as their journal get the real one, or none until it's known. This runs on import, when a library opens, after a lookup, and after contents are read. Fields set by hand are left alone, and the changes can be undone like any other.
- **Suggestions in Edit details.** Names of journals, publishers, places, conferences, institutions and so on, and people, are suggested from the library as they're typed, most used first.
- **A ? beside the search box** lists every way of searching, with an example of each that can be added to the search.

## Adding to a library, or replacing it

- **Adding (the default):** a publication is the same one if it has the same uuid, or if a file here has the same contents.
  - Where both libraries have it, the more recently edited version wins, and what it replaced is kept in its history, so it can be undone.
  - Merging only adds: anything here that the export lacks stays.
  - Tags and collections with the same name are joined. Highlights, places and summaries keep the more recent version.
  - Importing the same export twice changes nothing.
- **Replacing:** the library becomes a copy of the export. A complete backup is made first, beside the library, named `library-before-import-` and the date. Files the export doesn't account for are found again by the next scan, as new.
- **Parts:** publications (with read status, ratings, notes, summaries and history), tags and collections, highlights and notes, places in documents, and settings can each be left out.

## What's in an export

- **In it:** publications, articles inside issues and books, tags and their hierarchy, collections, highlights and notes, places in documents, kept summaries, each field's history, and settings.
- **Not in it:**
  - **Your documents,** covers and the full-text index, which are made again on the other computer.
  - **Publications only kept for a folder you removed.**
  - **Online lookup results waiting for review.**
  - **The Claude API key,** which never leaves the credential store.
- **Settings:** settings for this computer alone, such as window sizes, aren't exported. Folder paths from the other computer are shown in the report rather than applied, since they're unlikely to exist.
- **Privacy:** the file isn't encrypted, and includes your notes and the contact email for lookups if you've entered one. The export says so before saving.
- **Format:** a zip of JSON files, one for each kind of record, with a version number. A file from a newer version is refused with a reason, a damaged one is noticed rather than half read, and a file missing a part still reads.

## Decisions I made

- **A publication found by its contents takes the export's details and uuid,** even though one made by scanning here is newer. The export is the library you looked after; the one here was only read from the file.
- **Open documents are closed during an import,** and indexing, lookups and scanning pause, so nothing else writes to the library meanwhile. The import is one transaction: it all happens or none of it does, so cancelling changes nothing.
- **Personal notes keep no history,** as before, so an older note replaced by a newer one can't be undone. Details can.
- **No new dependency:** the zip reading and writing is written here on zlib, which every platform already has.

## How it differs from the plan

- **Collections and kept summaries** are included, which came after the plan was written.
- **Suggested matches** have their own list under **File**, rather than joining the review queue for online lookups, since accepting one does something quite different.
- **"Reading an export from an older version"** is covered by `tests/fixtures/transfer/library-v1.papertrail`, an export made by this version and kept. Every later version's tests must still import it, on every platform.
- **Windows to Linux:** the same fixture is imported on Windows, Arch and Ubuntu, and nothing in an export depends on paths or the platform. A real export from one computer imported on another hasn't been tried.

## Problems testing found

- **Importing was slower than it should have been.** For each publication waiting for its file, looking for possible matches searched the whole library by lowercased identifier and by file name, which would grow with the square of the library's size. These now use the indexes the library already has.
- **The release build warned** that the zip reader's byte reads might run past the end. They now check, and give nothing past either end, which the reader treats as damage.
- **Some files I edited this session had Windows line endings,** from my own editing scripts. They're back to Linux line endings, and `.gitattributes` would have normalised them on commit anyway.

## Speed

On this PC with a release build and the synthetic 100,000-publication library (`test_transfer_perf`, labelled `perf`):

| Step | Time |
|---|---|
| Exporting | 22 s, to a 25 MB file |
| Reading the file | 1.1 s |
| Importing into an empty library | 69 s |
| Importing again, with nothing new | 21 s |

The time grows in step with the library's size, at about 13 s for 20,000. Most of an import is creating publications through the same code everything else uses, and updating the search index as tags are attached. Both show a progress bar and can be cancelled. The test's limits are a minute to export and two minutes to import.

## How it's been tested

- **Automated:** all test programs pass on all three platforms. The new `test_transfer` builds a library through the real import and worker, with a corrected title, rating, notes, nested tags, a collection, an article inside an issue, a highlight, a place, a summary, settings and a removed folder. It exports that library and imports it into another whose files are shuffled into other folders under other names. It covers both importing before the files are scanned and after, and checks:
  - that everything comes back, under the same uuids, with no duplicates
  - importing twice, and back again
  - which edit wins, with undo
  - replacing, with its backup
  - choosing parts
  - suggested files, and accepting one
  - damaged and newer files
  - the kept fixture
  - that the documents are never changed
- **The window:** `test_library_ui` covers exporting and importing from the window, the import dialog's choices, the report, replacing with its backup, the suggested files list, and finding a file by hand.
- **Not tested yet:**
  - **By hand,** including the dialogs' wording and layout.
  - **Two real computers,** including Windows to Linux.
  - **Your real library,** and how long it takes to export and import.

## Known limits

- **One import or export at a time,** and the library can't be used while one runs.
- **A file found by hand** must be in one of the library's folders.
- **Articles are only matched by uuid,** since they have no files of their own. One made separately on each computer comes through as two articles.
