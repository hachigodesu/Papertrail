# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Papertrail is a C++20 / Qt 6 Widgets desktop library manager and reader for academic publications, for Windows and Linux. It indexes files where they already are and stores metadata in SQLite. `docs/PLAN.md` is the approved plan (milestones M0 to M12). `docs/M*-results.md` record what each finished milestone delivered, how it differed from the plan, and what was carried forward, so read the latest one before starting a milestone.

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
cmake --build ~/papertrail-build
```

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir ~/papertrail-build -LE perf --output-on-failure
```

On Ubuntu, also pass `-DCMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64` when configuring. Windows PowerShell splits `-D` arguments at dots when calling `wsl.exe`, so run this from a script or a Linux shell. Don't edit a script while a job is running it.

Test on all three platforms before calling work finished. They behave differently: see the Arch crash under "Worker process".

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

Use `linux-release` and `linux-perf` on Linux. `test_query_perf` and `test_ui_perf` read `PAPERTRAIL_PERF_COUNT`, `PAPERTRAIL_PERF_DB` (reuse a library between runs) and `PAPERTRAIL_PERF_REPORT_ONLY` (never fail, for CI).

### Tools

- `papertrail-import --db X --cache Y [--worker W] [--max-jobs N] FOLDER...` runs an import without the GUI.
- `gen-library` builds synthetic libraries.
- `ui-preview --db X --cache Y --out DIR` saves pictures of the window, for checking how it looks. Use `QT_QPA_PLATFORM=offscreen`, which gives a poor fallback font but no window on the user's screen.
- `-DPAPERTRAIL_BUILD_FORMAT_PROBE=ON` adds `format-probe`, which tests document formats against the engines.
- `tests/make_import_samples.py OUTPUT_DIR` generates the documents in `tests/samples/`. They're committed as binary because tests depend on their exact bytes.
- `packaging/linux/build-appimage.sh` builds the AppImage, and `smoke-test-appimage.sh` tests it.

## Architecture

### Libraries and processes

| Target | Links | Role |
|---|---|---|
| `papertrail_core` | Qt Core, SQLite | Database, schema, models, queries, repositories, settings |
| `papertrail_engine` | MuPDF, DjVuLibre | Opens and renders documents, and extracts their text |
| `papertrail_import` | core | Scanning, the job queue, metadata extraction and classification, the worker client |
| `papertrail_ui` | core, import, Qt Widgets | The library window |
| `papertrail` | ui | The app |
| `papertrail-worker` | engine, import | The only part of the app that opens documents so far |

`papertrail_import` doesn't link the engine. It reaches documents only through the worker.

### Worker process

Parsing unknown files will eventually crash a parser, so it happens in a separate process. `WorkerClient` drives `papertrail-worker` with `QProcess`, using line-delimited JSON over stdin and stdout (ops: `hash`, `inspect`), and restarts it after a crash or timeout. A file that keeps failing is listed as unreadable under its file name, so it isn't lost.

This is a real risk: Arch's MuPDF hits a double free on `tests/samples/truncated.pdf`, while Ubuntu's and MSYS2's builds don't. The reader in M4 must account for this (see `docs/M2-results.md`). Don't assume a clean Windows run means a file is safe.

`__crash__` and `__hang__` in a file name trigger faults in the worker, only when `PAPERTRAIL_WORKER_TEST_FAULTS` is set.

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

`PublicationListModel` backs both views and holds only a few pages. `data()` returns nothing for rows it hasn't fetched and requests the page, so anything needing a row's id must handle `idAt()` returning null. A selection is by row, and rows can move when the library changes: `MainWindow::onResultsChanged` clears a selection it can no longer vouch for. Count selections with `selectedRowRanges`, not `selectedRows()`, which is slow at 100,000.

**Text from documents is untrusted.** Show it with `Qt::PlainText` or through the delegates' `drawLines`, and only follow links that pass `safeWebLink`.

`FolderWatcher` differs by platform on purpose: Linux uses inotify, but on Windows `QFileSystemWatcher` stops people renaming their own folders, so it scans periodically and when the window is activated instead.

### Database and queries

- `Database` wraps the SQLite C API directly rather than using Qt SQL, which keeps control of the FTS5 build. SQLite is pinned in `cmake/SQLite.cmake`. A nested `Transaction` becomes a savepoint.
- Schema changes are numbered migrations in `src/core/db/Schema.cpp`. Add a new one and leave existing ones unchanged.
- Publications, tags and annotations have permanent UUIDs so re-importing an export can't create duplicates, and publications can exist without a file. The M11 export matches files by content hash, so don't make anything depend on stored paths surviving.
- `LibraryQuery` turns a `PublicationFilter` into SQL. It materialises search matches once in `temp.search_match`, invalidated by `Database::changeStamp()`, and relies on per-sort indexes using `coalesce(year, ...)`. Measuring search speed needs a new `LibraryQuery` per run, or reused matches flatter the result.
- All settings go through the registry in `SettingsStore`, so exports include new ones automatically. Machine-specific paths are only suggested on import, never applied.

## Pitfalls

- **MuPDF:** don't use objects with destructors, or throw, inside `fz_try`. Its error handling jumps with `longjmp`.
- **moc:** it can't parse raw string literals containing XML in test files.
- **CMake:** `find_package` imported targets are directory-scoped, so each directory that uses them needs its own call.
- **PowerShell 5.1:** a here-string piped to `git commit -F -`, and `gh --jq` with `\(`, both fail. Use the Bash tool for these.
- **Console encoding:** the Windows console is cp932, so set `PYTHONUTF8=1` for Python scripts that print non-ASCII text.
