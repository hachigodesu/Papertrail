# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Papertrail is a C++20 / Qt 6 Widgets desktop library manager and reader for academic publications, for Windows and Linux. It indexes files where they already are and stores metadata in SQLite. `docs/PLAN.md` is the approved plan (milestones M0 to M12, with M6.5 for collections before M7, M9.5 for article summaries before M10, and M11.5 for features added before release). `docs/M*-results.md` record what each finished milestone delivered, how it differed from the plan, and what was carried forward, so read the latest one before starting a milestone.

## Rules

- **Never write paths to the user's own document folders into any file or commit,** and don't copy real paper titles or author names from their files into tests or test data. The repository goes public at V1. Ask for the path each time. On Windows, map it to a temporary drive with `subst`, not a junction (recursive deletes can follow a junction into the real files). Report only aggregate results, then remove the mapping and test library and search the repo to confirm the path isn't there.
- **Never modify the user's documents.** Annotations and metadata live only in Papertrail's database. Tests hash folders before and after to enforce this.
- **Ask before starting a milestone, and before each commit or push.**
- **Keep Windows on Qt 6.12,** the last release that supports Windows 10. CMake fails on Windows with 6.13 or newer, and MSYS2 holds the packages with `IgnorePkg`. Linux may use newer versions, and Arch does.
- **Keep MuPDF and DjVuLibre calls inside `src/engine/`,** behind the `Document` and `Engine` interfaces. The optional `tools/format_probe` is the one exception.
- **The licence is AGPL-3.0-or-later,** so check that any new dependency is compatible.

## Commands

There's no linter or formatter configured. GCC and Clang build with `-Wall -Wextra -Wpedantic`.

### Windows

Use MSYS2 UCRT64 at `C:\msys64`. Its `bin` folder must come first on `PATH`: another `cmake` on this PC (Strawberry's) is found first otherwise, and the test programs need MSYS2's DLLs.

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
```

```bash
cmake --preset windows-debug
```

```bash
cmake --build --preset windows-debug
```

```bash
ctest --preset windows-debug
```

Run one test program:

```bash
ctest --preset windows-debug -R test_import
```

Run one test function. On Windows, QtTest prints nothing to the console unless `QT_FORCE_STDERR_LOGGING=1` is set, and it still exits with the right code:

```bash
QT_FORCE_STDERR_LOGGING=1 ./build/windows-debug/test_import.exe functionName
```

`-functions` lists a program's test functions. Windows executables land in the root of `build/<preset>/`. On Linux they're in `src/app/` (the app and its worker), `tests/` and `tools/<name>/` under the build folder. Don't run `papertrail` itself to test something: it opens the user's real library. Tests and `ui-preview` use their own.

### Linux (WSL2)

The distros are `Papertrail-Arch` (user `dev`) and `Papertrail-Ubuntu` (user `ubuntu`). See `docs/wsl-dev-setup.md` for their setup, including the DNS workaround. Keep build output in the Linux filesystem rather than on `/mnt/c`:

```bash
cmake --preset linux-debug -B ~/papertrail-build
```

```bash
cmake --build ~/papertrail-build -j 8
```

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir ~/papertrail-build -LE perf --output-on-failure
```

On Ubuntu, also pass `-DCMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64` when configuring. Windows PowerShell splits `-D` arguments at dots when calling `wsl.exe`, so run this from a script or a Linux shell. Don't edit a script while a job is running it.

Build on one distro at a time, with the job limit above. Building on both at once, with no limit, hung the shared WSL VM until `wsl --shutdown`. Test on all three platforms before calling work finished. They behave differently: see the Arch crash under "Worker process".

### Performance tests

These are labelled `perf` and excluded from the normal test presets. They take minutes on a 100,000-publication library, with a 100 ms target for results (250 ms for ranked search). They need a release build:

```bash
cmake --preset windows-release
```

```bash
cmake --build --preset windows-release
```

```bash
ctest --preset windows-perf
```

Use `linux-release` and `linux-perf` on Linux. `test_reader_perf` times a generated 3,000-page book. `test_query_perf` and `test_ui_perf` read `PAPERTRAIL_PERF_COUNT`, `PAPERTRAIL_PERF_DB` (reuse a library between runs) and `PAPERTRAIL_PERF_REPORT_ONLY` (never fail, for CI).

### Tools

- `papertrail-import --db X --cache Y [--worker W] [--max-jobs N] FOLDER...` runs an import without the GUI.
- `gen-library` builds synthetic libraries.
- `papertrail-lookup --doi X` (or `--isbn`, `--arxiv`, `--title`) asks the real services and prints what the app's parsers make of it. Use public records only, never identifiers or titles from the user's files.
- `ui-preview --db X --cache Y --out DIR` saves pictures of the window, for checking how it looks. Use `QT_QPA_PLATFORM=offscreen`, which gives a poor fallback font but no window on the user's screen.
- `-DPAPERTRAIL_BUILD_FORMAT_PROBE=ON` adds `format-probe`, which tests document formats against the engines.
- `tests/make_import_samples.py OUTPUT_DIR` generates the documents in `tests/samples/`. They're committed as binary because tests depend on their exact bytes, so don't regenerate them all: write only a new one. The exception is `kf8_frankenstein.azw3`, a real Project Gutenberg book, because nothing here can make a KF8 file. `tests/KindleSamples.h` builds joint and DRM-marked Kindle files from it at test time.
- `packaging/linux/build-appimage.sh` builds the AppImage, and `smoke-test-appimage.sh` tests it.

## Architecture

### Libraries and processes

| Target | Links | Role |
|---|---|---|
| `papertrail_core` | Qt Core, SQLite | Database, schema, models, queries, repositories, settings |
| `papertrail_engine` | MuPDF, DjVuLibre | Opens and renders documents, and extracts their text |
| `papertrail_import` | core | Scanning, the job queue, metadata extraction and classification, the worker client |
| `papertrail_metadata` | core, import, Qt Network | Online sources, matching, the lookup engine |
| `papertrail_summary` | core, metadata | Article summaries from Claude |
| `papertrail_citation` | core | Citation styles, and BibTeX, RIS and CSL-JSON export |
| `papertrail_transfer` | core, zlib | Library export and import between computers |
| `papertrail_ui` | core, import, metadata, summary, citation, transfer, QtKeychain, Qt Widgets | The library window |
| `papertrail` | ui | The app |
| `papertrail-worker` | engine, import | The only part of the app that opens documents |

`papertrail_import` and `papertrail_ui` don't link the engine. They reach documents only through the worker.

### Worker process

Parsing unknown files will eventually crash a parser, so it happens in a separate process. `WorkerClient` drives `papertrail-worker` with `QProcess`, using line-delimited JSON over stdin and stdout (ops: `hash`, `inspect`, `pagetext`, `cover`, `markedcopy`), and restarts it after a crash or timeout. A file that keeps failing is listed as unreadable under its file name, so it isn't lost.

This is a real risk: Arch's MuPDF hits a double free on `tests/samples/truncated.pdf`, while Ubuntu's and MSYS2's builds don't. Don't assume a clean Windows run means a file is safe.

`__crash__` and `__hang__` in a file name trigger faults in the worker, only when `PAPERTRAIL_WORKER_TEST_FAULTS` is set. For the reader, so do `__crashpageN__` and `__hangpageN__`, on page N.

### Reader

`DocumentClient` runs one `papertrail-worker --reader` process (`src/worker/ReaderHost.cpp`) per open document, so nothing in the window ever parses a document. Replies are a JSON line, optionally followed by raw bytes, so stdout has to stay binary-clean: the host takes a private copy of it and points the original at stderr. Nothing in the client waits. It sends one request at a time, in order of importance: urgent requests, selections, the wanted pages (replaced wholesale as the view scrolls, so skipped pages are never rendered), thumbnails, then background work such as measuring pages and searching. If the process dies mid-request, that request's page is marked bad for the session and the process restarts.

`PageView` paints, and holds only pages near the view. Page colours are applied at paint time. `setPageRange` limits it to some pages, which is how an article opened from the library shows only its own: everything that counts pages goes through `rowStart` and `shownEnd` so the range holds. `ReaderTab` holds the toolbar and side panel, and saves the place in `reading_state`. Store a place with `placeForPage`, not `bookmarkForPage`: MuPDF's EPUB bookmarks are only valid inside one process.

### Import pipeline

`ImportService` holds work in the database's `job` table (`fingerprint`, then `inspect`), so stopping and re-running carries on. A partial unique index allows one active job per kind and file. Duplicates are matched by content hash, so one publication can have several file locations. `linkToKnownContents` also runs just before inspect, to catch copies that were queued at the same time.

`createPublication` chooses evidence in a fixed order, and anything guessed is marked for review. For titles, that's embedded metadata unless it's junk, then first-page text, then the file name. Identifiers are only believed near the start of a document, and a document's type is decided firmly only from its first page, because reference lists mention other theses, reports and conferences. Heuristics in `src/import/Extractors.cpp` came from real-file failures: when changing them, add a made-up case with the same pattern to `test_extractors`.

### Library window threads

The window's thread never waits on slow work, and each background thread opens its own `Database` connection (WAL allows reads during writes, with a 5 s busy timeout on writes). `LibrarySession` holds the window thread's connection, and takes a lock file so only one copy can open a library.

| Thread | Does |
|---|---|
| `ListQueryThread` | The list's total and pages. The newest request is served first, and results for an old generation are dropped |
| `SidebarQueryThread` | Filter counts, which take a few hundred ms at 100,000. A newer request abandons one in progress |
| `ImportThread` | Scans and the job queue. Its `cancelled` check also stops the worker mid-job, without counting against the file |
| `BulkChanges` | Changes to many publications, in batches so other writers only wait briefly |
| `CoverCache` pool | Decodes covers, newest request first |
| `IndexThread` | Reads document text into the full-text index, through the worker |
| `ArticleCoverThread` | Makes an article's cover from its first page, through the worker's `cover` op, the first time it's shown |
| `LookupThread` | Runs `LookupEngine` through the queued online lookups |

Tests that show a window should fail on a `QMessageBox`, which would otherwise wait forever for a click: see `m_boxWatcher` in `test_reader.cpp`.

`PublicationListModel` backs both views and holds only a few pages. `data()` returns nothing for rows it hasn't fetched and requests the page, so anything needing a row's id must handle `idAt()` returning null. A selection is by row, and rows can move when the library changes: `MainWindow::onResultsChanged` clears a selection it can no longer vouch for. Count selections with `selectedRowRanges`, not `selectedRows()`, which is slow at 100,000.

**Text from documents is untrusted.** Show it with `Qt::PlainText` or through the delegates' `drawLines`, and only follow links that pass `safeWebLink`.

`FolderWatcher` differs by platform on purpose: Linux uses inotify, but on Windows `QFileSystemWatcher` stops people renaming their own folders, so it scans periodically and when the window is activated instead.

### One publication, several formats

`publication_file` links a publication to any number of files, and `is_primary` says which supplies the cover. `ImportService::findOtherFormat` decides whether a new file is another format of something already there, and never joins two files of one format. It's deliberately cautious, and anything less than certain gets marked for review: an earlier attempt at automatic sorting into Preprints put published papers there unflagged, and the user had it removed. `mergePublications` and `separateFormat` in `core/repo/PublicationMerge` do the joining and undoing.

KF8 Kindle files (`.azw3`, and joint `.mobi` files) look like valid documents to MuPDF, which then shows a blank page. So `readablePath` in `ReaderHost.cpp` sends them through `convertKf8ToEpub`, which uses libmobi (`cmake/Mobi.cmake`) in the worker and keeps the EPUB in the cache folder, named by the file's content hash. Everything after that treats it as an EPUB.

A reading place belongs to a format, not to a publication. Use `ReadingStateRepository::placeFor` and `setPlaceFor`.

### Online lookups

`src/metadata/Transport` is the only way anything reaches the network, and tests swap it. Offline mode, and a library where no mode has been chosen yet (`metadata.mode_chosen`), never queue a lookup. `test_metadata` checks this using the real network code against a server inside the test, so keep it that way. Only identifiers and titles are ever sent, and the User-Agent says nothing about the user. A contact email goes only to Crossref and OpenAlex, and only if the user enters one.

Lookups are `job` rows of kind `lookup`, which belong to `LookupRepository` and not `ImportService`. `LookupEngine::decide` holds the table of what each mode does with a result. `plannedChanges` in `Matching.cpp` decides what a result may change: it leaves fields set by hand alone, only fills gaps in DOI, ISBN and abstract, and can never move anything to or from Preprint. Sources can only suggest that (`preprintHint`).

Rate limits differ by source and by kind of request, and OpenAlex's anonymous searches run out at about 100 a day. A 429 blocks that source alone, and the queue only waits when no source could answer. A failure of the network or a source puts the job back without counting it as an attempt.

Retractions and other notices come from Crossref (`metadata/NoticeChecker`, `core/repo/NoticeRepository`), which `LookupThread` runs after any lookups, only when lookups are online and `metadata.check_notices` is on. It sends DOIs only, 20 to a request, each publication about once a month. A run never checks one publication twice, so the cut-off for "due" is never later than the run's start.

### Citations

`citation::citationItems` turns publications into `CitationItem`s, taking what an article or chapter lacks (its journal, book, editors, publisher) from what it's in. `Styles.cpp` holds the four styles, written out by hand rather than through CSL: Harvard (following rules the user chose, which mustn't be named or quoted anywhere), APA 7th, IEEE and Chicago author-date (18th). `formatReferences` sorts, numbers and adds 2020a/2020b letters across a list, so format a list in one call rather than item by item. Each style's output for every kind of publication is fixed in `tests/fixtures/citations/`, written by hand from the rules, and a change to a style should change those by hand too. `test_citations` also runs pdflatex, BibTeX and biber over an export where they're installed (the Ubuntu WSL distro has them). `CitationActions` is the window's side: copying with italics as HTML and plain text, and exporting on its own thread and database connection.

### Library export and import

A `.papertrail` file is a zip (`transfer/Zip`, on zlib) of versioned JSON, one file per kind of record, with `manifest.json` giving the version. Everything is linked by uuid, and files are described only by SHA-256, size, format and name, never by path, so add nothing path-like to it. Publications in a removed folder stay out. `importBundle` runs in one transaction: a publication is matched by uuid, or else by a file here with the same contents, in which case it takes the export's uuid and details. Where both have it, the later `updatedAt` wins, through `updateWithProvenance` so it can be undone, and merging only adds. Files are linked by hash; anything else waits in `expected_file`, and `FileRepository::linkWaitingPublication` connects it when the import hashes a matching file. Uncertain matches (same identifier, or same name and size) go in `file_proposal` for the user. Replace makes a `VACUUM INTO` backup first. `tests/fixtures/transfer/library-v1.papertrail` is an export kept so later versions must still read it: don't regenerate it, and add a new one if the format changes.

### Article summaries

`src/summary` builds requests for Claude, reads replies and works out costs (`SummaryRequest`), and drives one summary with `Transport::send` (`SummaryEngine`), carrying on after `pause_turn`. It doesn't use an SDK, since there isn't one for C++. `SummaryDialog` holds the steps. Nothing about a publication goes before the user confirms, and the paper needs a confirmation of its own, so estimates are worked out locally and never by asking Anthropic. The key lives only in `ApiKeyStore` (QtKeychain, built from `cmake/Keychain.cmake`), never in the database or settings, because exports include every setting. `Transport::send` doesn't follow redirects, so a key can't reach another host. Prices are in `knownModels()`: update them and `pricesCheckedOn()` together. Never make a real API call in a test.

Batches at half price (`summary/SummaryBatches`) keep their state in `summary_batch` and `summary_request`, so closing the app loses nothing, and the window checks every few minutes while any wait. Collecting a batch notes every result in one transaction. A result that paused is noted as `to_continue` before it's carried on in a new batch, and one that arrived waits as `arrived` until the user keeps or discards it in `SummariesDialog`. Only keeping it touches `publication_summary`. They only ever send identifying details, never the paper.

`summary_spend` (`summary/SummarySpending`) notes what each reply cost as it comes back, whether instant or batched and whether or not it's kept, keyed so noting it twice changes nothing. It keeps the title, since publications can go. Wherever the window names the Claude Console, `consoleLabel` makes it a link to a fixed Console address. A batch id goes into later addresses, so `parseBatch` only accepts Anthropic's characters, and the results address is built rather than taken from a reply.

### Highlights and notes

`AnnotationRepository` keeps these by content hash, so they survive a file moving. Fixed pages store a page and rectangles. Reflowable text stores the words, a place and which repeat it was, because rectangles wouldn't survive a layout change: `ReaderTab::locateAnnotations` asks the document's process to find them again (the `anchor` and `locate` ops) each time the layout changes. `PageView` only draws the marks it's given.

`annotation_fts` and `publication_note_fts` let searches find highlights and notes (`note:`, and plain words too); triggers keep them up to date. When some matches come only from these, ranking by relevance is skipped, as for document contents. A marked copy of a PDF (`ui/MarkedCopy`, the worker's `markedcopy` op, `Document::saveMarkedCopy`) is written as a new file from a freshly opened document, and refuses the original's path.

### Full-text search

Document text is in `fulltext.db`, attached to a connection as `fulltext` by `attachFullText` (`LibrarySession` and the query threads do this, and `core/repo/FullTextIndex` holds the rest). It's kept by content hash, and each page's row number is its document's id times `kFullTextPagesPerDocument`, plus the page, so that looking within one document is quick however common the word is. `FullTextIndexer` needs no list of work: what's left is always whatever files aren't indexed yet. It runs on `IndexThread` and reads through the worker's `pagetext` op, so stopping is safe and a bad file costs only that file. `PublicationFilter::searchContents` adds these matches to the search, and `ParsedSearch::contentsMatch` is the part of a search that documents can answer, since they have no fields.

### Volumes and articles

An article is a publication with `parent_id`, `page_start` and `page_end`, and no file of its own: anything reading a file goes through `coalesce(parent_id, id)`, as `kOwnerOfFile` does. Those pages are positions in the file, not printed page numbers, so they don't go in the citation `pages` field. `core/repo/VolumeArticles` works out articles from a table of contents, and `ContentsDialog` reads the contents through a `DocumentClient`, so a bad file can't take the window down. Where the contents only number an entry, the dialog reads that page's largest text and any DOI with the worker's `heading` op, which also finds the issue's printed numbers (`findIssueNumbering`). An article's cover key is its file's hash and first page (`articleCoverKey`), and `CoverCache` shows the file's cover until it's made. The code says "volume" for anything holding articles, but the journal type is `PublicationType::JournalIssue`, shown as "Journal issue" and stored as `journal_volume`, its first name.

Without a table of contents, `ContentsDialog` reads every page's heading (largest text and DOIs) and `import::draftsFromPages` starts an article wherever a page has a new DOI from the issue's main publisher. It needs at least two, since one is most likely a whole paper.

An issue's title from a file is usually its cover's largest text. `import/JournalIssues` (`tidyJournalIssue`) recognises titles that only describe an issue, fills in its numbers and journal, and retitles it from them, never touching fields set by hand. It runs on import, when a library opens, after lookups and after contents are read, so call it from anything new that changes an issue or its articles.

### Collections

`CollectionRepository` holds named groupings, which can sit inside one another (`collection_parent`, a table of its own so the migration can run again). They're a filter like any other: `PublicationFilter::collectionIds` narrows every query, including the collections inside those chosen, and `FilterSidebar` lists them as a tree with counts from `LibraryQuery::collectionCounts`. Adding an issue adds its articles, and new articles join their issue's collections.

### Removed folders

Removing a folder (`FileRepository::hideRoot`) adds it to `removed_root` and keeps its file records, so adding the same path again (`restoreRoot`, then a scan) brings everything back. `roots()` only returns folders being indexed. Publications that are only in a removed folder, and articles inside them, go in `hidden_publication`, and `LibraryQuery` leaves those out of everything. Having no file doesn't mean hidden: a publication imported from another computer may be waiting for its file.

A trigger on `publication_file` brings a hidden publication back whenever it gets a real file, so the same contents found in another folder reconnect by hash. It also drops the old location. Anything that lists what's waiting, or sends something online, has to leave hidden publications out too.

Publications imported from another computer that are still waiting for their files (`expected_file`, see below) are hidden the same way, so the same trigger brings them back. `hiddenCount` and `hiddenPublications` mean only those kept for removed folders, and the export includes waiting ones.

### Removed publications and backups

Removing a publication (`core/repo/RemovedContent`) deletes it and remembers its files by contents in `removed_content` (by file id for an unreadable or empty one). `ImportService::createPublication` and fingerprinting both check it, so nothing removed comes back by any route until brought back, which clears the files' hashes so the next scan reads them as new. Exports carry the list by contents.

`core/db/Backups` makes copies with `VACUUM main INTO` in a `backups` folder beside the library: daily on a thread (`DailyBackupThread`), before an upgrade in `LibrarySession` (the library isn't opened if that fails), before Replace, before a restore, and by hand. Restoring uses `Database::replaceWith`, SQLite's backup API on the open connection, which other connections see; `replaceWith` bumps `changeStamp` itself, since it isn't counted as a change.

### Database and queries

- `Database` wraps the SQLite C API directly rather than using Qt SQL, which keeps control of the FTS5 build. SQLite is pinned in `cmake/SQLite.cmake`. A nested `Transaction` becomes a savepoint.
- Schema changes are numbered migrations in `src/core/db/Schema.cpp`. Add a new one and leave existing ones unchanged. Each must be safe to run again (`IF NOT EXISTS`, `INSERT OR IGNORE`), since tests rewind a library's version and migrate it again: so no `ALTER TABLE ... ADD COLUMN`, which can't be.
- Publications, tags and annotations have permanent UUIDs so re-importing an export can't create duplicates, and publications can exist without a file. The M11 export matches files by content hash, so don't make anything depend on stored paths surviving.
- `LibraryQuery` turns a `PublicationFilter` into SQL. It materialises search matches once in `temp.search_match`, invalidated by `Database::changeStamp()`, and relies on per-sort indexes using `coalesce(year, ...)`. Measuring search speed needs a new `LibraryQuery` per run, or reused matches flatter the result.
- All settings go through the registry in `SettingsStore`, so exports include new ones automatically. Machine-specific paths are only suggested on import, never applied.

## Pitfalls

- **MuPDF:** don't use objects with destructors, or throw, inside `fz_try`. Its error handling jumps with `longjmp`.
- **moc:** it can't parse raw string literals containing XML in test files.
- **CMake:** `find_package` imported targets are directory-scoped, so each directory that uses them needs its own call.
- **PowerShell 5.1:** a here-string piped to `git commit -F -`, and `gh --jq` with `\(`, both fail. Use the Bash tool for these.
- **Console encoding:** the Windows console is cp932, so set `PYTHONUTF8=1` for Python scripts that print non-ASCII text.
