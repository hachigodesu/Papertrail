# Papertrail: V1 build plan

## Context

University students and researchers keep large folders of papers, textbooks, theses, reports and standards, and these are hard to search or organise. Papertrail is a Windows and Linux desktop app, written in C++, that indexes those files **where they already are**. It stores rich metadata and user tags in a local database, shows everything in a tabbed, filterable library, and opens documents in a built-in reader.

The project folder is empty, so this is a new build.

### Decisions you made

| Topic | Decision |
|---|---|
| Distribution | Open source, AGPL-3.0-or-later |
| Formats | PDF, EPUB, DjVu, MOBI/AZW3, XPS, CBZ, FB2 |
| V1 research features | Full-text search, highlights and notes, citation export |
| Metadata | Users choose between all three modes: online lookup with review, online lookup without review, offline only |
| Journal volumes | A volume is one entry, with child articles defined by page range |
| Library location | One library per computer |
| Moving between computers | Export and import of all database information and settings, which works even when files sit in a different folder structure. No cloud sync |
| OCR | Not in V1. Scanned files are flagged as "no text" |
| Hosting | Private GitHub repository, made public at V1 |
| Database | SQLite |
| Linux support | Usable on all distros, with full testing on both Arch and Ubuntu |
| Citation styles | Includes Harvard, alongside APA, IEEE and Chicago |
| Later roadmap | Zotero/Mendeley/BibTeX import, OCR |

### Defaults I chose (tell me if you disagree)

- **Your files are never modified.** Annotations and metadata live only in Papertrail's database.
- **Duplicates merge.** The same file found in two folders becomes one publication with two locations, matched by content hash.
- **Tags are hierarchical**, for example `Engineering > Structures > FEM`. A flat tag is just one with no parent.
- **The library has two views:** a cover grid and a detailed list, switched with a toggle.
- **Design target:** 100,000 publications stays responsive.
- **Online lookups send identifiers or titles only.** File contents are never uploaded.

---

## Technology

| Layer | Choice | Why |
|---|---|---|
| Language and build | C++20, CMake with presets, Ninja | Same build on both platforms |
| UI | Qt 6.12 Widgets | Native desktop feel, and its model/view classes handle very large lists |
| PDF, EPUB, XPS, CBZ | MuPDF (AGPL) | One fast engine with rendering, text extraction and search built in |
| DjVu | DjVuLibre | The standard DjVu library |
| MOBI, FB2 | MuPDF | Confirmed in M0, though MuPDF's site doesn't list them |
| AZW3 (KF8) | libmobi v0.12 (LGPL-3+) converts to EPUB, then MuPDF | MuPDF alone gives a blank page. See `docs/M0-format-results.md` |
| Database and search | SQLite with FTS5, used directly rather than through Qt SQL | Controls the FTS5 build and needs no server |
| Networking | Qt Network | Metadata lookups |
| Tests | Qt Test, with `-platform offscreen` for UI smoke tests | Already part of Qt |

No embedded browser (Qt WebEngine) is needed, which keeps the install much smaller.

### Toolchain

- **Windows:** MSYS2 UCRT64 (GCC, Qt, DjVuLibre and MuPDF packages). The compilers on this PC aren't suitable: `C:\MinGW` is GCC 6.3 (32-bit) and Strawberry's GCC has no Qt. Both are on `PATH`, so the CMake presets will pin the compiler explicitly.
- **Linux development and testing:** Arch and Ubuntu 24.04 LTS, both in WSL2 on this PC and both in GitHub Actions.
  - **Arch** uses its own packages for Qt, MuPDF and DjVuLibre.
  - **Ubuntu** uses Qt 6.12 from `aqtinstall` and MuPDF built from source at a pinned version, because its own packages are older.
- **Linux release builds:** built separately on an older base system with Qt 6.12 from `aqtinstall`, so the packages run on as many distros as possible. See "Linux support" below.
- **Hold Qt at 6.12 on Windows.** This matters for your PC too (see "Findings").

### Linux support

The two test platforms cover both ends of the range. Arch is a rolling distribution and finds problems with new library versions early, while Ubuntu LTS represents the slower-moving distros many students use. Software built on Arch won't run on older distros, so release packages are built separately on an older base.

| Package | Covers | Notes |
|---|---|---|
| **AppImage** | Most glibc distros: Ubuntu, Debian, Fedora, openSUSE, Arch, Mint and so on | Bundles Qt, MuPDF and DjVuLibre. M0 sets the oldest supported glibc, which depends on what Qt 6.12's binaries require. Uses a runtime that doesn't need `libfuse2`, which recent Ubuntu releases don't install by default |
| **Flatpak** | Immutable and non-glibc distros such as Fedora Silverblue and Alpine, and distros older than the AppImage baseline | Moved into V1 from the roadmap. Needs broad file access, because Papertrail indexes files wherever they already are |
| **AUR package** | Arch users | Native build from a `PKGBUILD` |
| **Source build** | Everything else | Documented build steps, and no hard-coded distro paths |

Code rules that keep it distro-neutral:

- Use standard XDG folders through `QStandardPaths`.
- Support both Wayland and X11.
- Don't assume a particular desktop environment.
- Treat file paths as case-sensitive.
- Keep the code compiling against newer Qt versions as well as 6.12. The native Arch build catches breakage as Arch updates, and the Ubuntu build keeps us honest on the pinned versions.

### Findings from checking these dependencies

- **Qt 6.12 is the last version to support Windows 10**, according to Qt's platform page. MSYS2 is a rolling distribution, so we hold the Qt packages at 6.12 (`IgnorePkg`) and in CI. Moving past it would drop Windows 10 users, including this PC.
- **MOBI, AZW3 and FB2 were tested in M0.** MuPDF handles MOBI and FB2, but silently gives a blank page for AZW3. libmobi converts AZW3 to EPUB successfully. It isn't in MSYS2, so it's built from a pinned source version on every platform.
- **MSYS2 and Arch currently match closely:** both have MuPDF 1.28.5 and Qt 6.12.0, and DjVuLibre is 3.5.30 on MSYS2 and 3.5.30.1 on Arch. I couldn't confirm that the `libmupdf` packages include development headers. If they don't, we build MuPDF from source at a pinned version.
- **Arch on WSL2 is unconfirmed.** The Arch wiki page wouldn't load for me, so M0 confirms the install route. It also checks whether GUI apps from Arch and Ubuntu display on this Windows 10 PC. If they don't, we use VMs.

---

## Architecture

```
┌────────────────────────── papertrail (GUI) ───────────────────────────┐
│  Library UI          Reader UI          Review queue       Settings   │
├───────────────────────────────────────────────────────────────────────┤
│  Core: repositories · query builder · tags · job queue · citations    │
├──────────────┬─────────────────────┬──────────────────────────────────┤
│  library.db  │  fulltext.db        │  Metadata providers              │
│  (SQLite)    │  (SQLite FTS5)      │  Crossref · OpenAlex · arXiv ·   │
│              │                     │  Open Library                    │
├──────────────┴─────────────────────┴──────────────────────────────────┤
│  DocumentEngine interface:  MuPDF · DjVuLibre · MOBI adapter          │
└───────────────────────────────────┬───────────────────────────────────┘
                                    │ jobs, over a local socket
                     papertrail-worker (separate process)
              covers · text extraction · embedded metadata · hashing
```

- **Bulk file parsing runs in a separate worker process.** Importing thousands of unknown files will eventually hit one that crashes a parser. The worker restarts, the file is marked unreadable, and the app stays up.
- **The reader opens files inside the app** on background threads. If the reader itself meets a bad file, it shows an error for that file.
- **All engines sit behind one `DocumentEngine` interface:** page count, render, text, outline, search and metadata. The UI never calls MuPDF directly.

### Storage

- `library.db` and `fulltext.db` live in the user's app data folder, and covers live in the cache folder. Keeping full text in its own database means `library.db` stays small and quick to back up.
- All settings are stored through one central registry, so every setting is included in an export automatically, including ones added later.

### Data model

- **Stable IDs:** publications, tags and annotations each carry a permanent UUID, so importing the same export twice never creates duplicates.
- **Publications can exist without a file.** Their metadata and tags are kept until a matching file turns up.
- `library_root`: a named scan location with its absolute path. If it's unavailable, such as an unplugged drive, its entries show as *offline* rather than deleted, and a root can be repointed to a new path.
- `file`: root, relative path, size, modified time, SHA-256, format, page count, has-text-layer flag and status (`ok`, `missing`, `offline`, `unreadable`).
- `publication`: the core record.
  - **Type:** journal volume, journal article, conference proceedings, conference paper, textbook, book, book chapter, thesis, dissertation, technical report, standard, preprint, other.
  - **Common fields:** title, subtitle, abstract, date, language, URL, edition, series.
  - **Identifiers:** DOI, ISBN-10/13, ISSN/eISSN, arXiv ID, PMID.
  - **Container fields:** journal or container title, volume, issue, pages, publisher, place.
  - **Conference:** name and location.
  - **Thesis:** institution, department, degree.
  - **Report:** report number.
  - **Standard:** standard number, issuing body, revision, status.
  - **Personal:** read status, rating, notes.
  - **Overflow:** a JSON field for anything else.
- `publication_file`: links publications to file locations, so one publication can have several copies.
- **Child articles:** `publication.parent_id` plus `page_start` and `page_end`. An article points at its volume and opens at the right page.
- `person` and `publication_person`: role (author, editor, translator, advisor) and position in the author list.
- `tag` (with `parent_id` and colour) and `publication_tag`.
- `metadata_provenance`: for each field, its source, confidence and previous value. This makes any automatic change reversible.
- `annotation`: publication, content hash, location, type, colour, selected text and note. Using the hash means annotations survive a file being moved or renamed.
- `reading_state`: last location, zoom and last opened.
- `job`: a persistent import queue, so a large import resumes after the app is closed.
- FTS5 tables: `publication_fts` (metadata, authors, tags) and `page_text` (per-page document text, with snippets).

Schema changes go through numbered migrations, with a test for each.

### Import pipeline

Each step is a resumable job. The steps run in the background and can be paused while you keep using the app.

1. **Scan:** walk the chosen folders and register supported files. Rescans happen at startup and on demand, and watched folders are picked up automatically. Linux limits how many folders can be watched, so startup rescans remain the safety net.
2. **Fingerprint:** hash each file, then detect duplicates and moved files.
3. **Extract:** read PDF Info/XMP and EPUB OPF metadata, and search the first pages for DOI, ISBN and arXiv patterns.
4. **Classify:** make a best guess at the publication type from identifiers and page text.
5. **Enrich:** act according to the metadata mode below.
6. **Cover:** render the first page to a cached thumbnail.
7. **Index:** extract page text into `fulltext.db`, or flag the file as "no text".

### Metadata modes

The user sets a default in Settings and can override it for each import.

| Mode | Behaviour |
|---|---|
| Online with review | Exact DOI, ISBN or arXiv matches apply automatically. Title-based or uncertain matches wait in a review queue with a side-by-side comparison |
| Online, no review | The best match always applies and is marked auto-matched, with one-click undo through provenance |
| Offline only | Embedded data and manual entry only. No network requests at all |

**Sources:** Crossref (DOI and articles), OpenAlex (articles, theses, fallback), arXiv (preprints), Open Library (ISBN and books). Lookups are rate-limited and cached. Crossref's "polite pool" asks for a contact email, which stays optional and off unless the user enters one in Settings.

### Library export and import

**What an export contains.** One `.papertrail` file, which is a zip of versioned JSON:

- Publications, metadata and provenance
- Tags and the tag hierarchy
- Child articles
- Highlights and notes
- Read status, ratings and reading positions
- All settings
- A fingerprint for each file: SHA-256, size, format and original filename

**What it leaves out.** Your documents are never included. Covers and the full-text index are rebuilt on the importing computer, which keeps the export small. JSON means an export from an older version can still be read after the database schema changes.

**How it copes with a different folder structure.** Files are matched by content, not location, so stored paths are never needed. The importing user picks their folders and Papertrail scans them, then works through these steps for each publication:

| Step | Match | Result |
|---|---|---|
| 1 | Same SHA-256 | Linked automatically, wherever the file is and whatever it's called |
| 2 | Same DOI, ISBN or arXiv ID, but different file contents (for example, another download of the same paper) | Proposed in the review queue. Annotations are kept but marked "may not line up" |
| 3 | Same filename and size | Proposed in the review queue |
| 4 | No match | Kept as "No file linked", with all tags and metadata intact. Links automatically if a later scan finds the file, and can also be linked by hand |

An import report shows how many publications were linked, proposed or left unmatched.

**Import options:**

- **Parts to bring in:** any of metadata, tags, annotations, reading state and settings.
- **Merge** (the default):
  - Tags and annotations from both libraries are combined.
  - If the same field differs, the most recently edited publication wins, and the earlier value stays undoable through provenance.
  - Something removed on one computer isn't removed on the other, because merging only adds.
- **Replace:** the library becomes an exact copy of the export. Papertrail makes an automatic backup first.

**Settings:**

- All settings transfer, including between Windows and Linux.
- Saved folder paths from the other computer are shown as suggestions and never applied automatically, because they usually won't exist on the new computer.

**Privacy:** exports aren't encrypted and contain your notes, plus the Crossref contact email if you've set one. The export dialog says so.

---

## User interface

### Library

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Papertrail   [Search: author:knuth year:>2010 tag:fem     ] [+Import]   │
├─────────────────────────────────────────────────────────────────────────┤
│ All │ Volumes │ Articles │ Conference │ Textbooks │ Theses │ Reports │ Standards │
├──────────────┬───────────────────────────────────────┬──────────────────┤
│ FILTERS      │ ┌─────┐ ┌─────┐ ┌─────┐  [Grid|List]  │ DETAILS          │
│ ▾ Tags       │ │cover│ │cover│ │cover│  Sort: Year ▾ │ Title, authors   │
│   ▸ Eng (41) │ │     │ │     │ │     │               │ Journal, DOI ... │
│ ▾ Year       │ └─────┘ └─────┘ └─────┘               │ Tags: + add      │
│   2005──2024 │ Title    Title   Title                │ File location    │
│ ▸ Author     │ Author   Author  Author               │ [Open] [Cite]    │
│ ▸ Journal    │ 2019     2021    2008                 │ [Edit metadata]  │
│ ▸ Publisher  │                                       │                  │
│ ▸ Format     │                                       │                  │
│ ▸ Read state │                                       │                  │
└──────────────┴───────────────────────────────────────┴──────────────────┘
```

- **Tabs:** All, Journal Volumes, Journal Articles, Preprints, Conference Papers, Textbooks, Theses & Dissertations, Technical Reports, Standards, Other.
- **Filter sidebar:** every filter shows live counts. Values within one filter combine with OR, and different filters combine with AND. Filters cover tags, year range, author, journal, publisher, conference, institution, language, format, read status, needs review, no text, no file linked, and missing or offline files.
- **Search box:** plain text, or field searches such as `author:`, `tag:`, `year:>2010`, `doi:`. A scope switch extends the search to document contents.
- **Grid and list:** both use virtualised models and load covers lazily.
- **Bulk actions:** tag, edit a field, set read status, re-run lookup, export citations.
- **Volumes:** expand to show their child articles. "Create articles from table of contents" proposes entries for review, and articles can also be added by hand.

### Reader

- Opens each document in a tab within the same window.
- Single-page, two-page and continuous scrolling, with zoom and fit modes.
- Table of contents and page thumbnails.
- Search within the document, with text selection and copy.
- Font size and re-layout for EPUB and other reflowable formats.
- Light, dark and sepia page modes.
- Resumes where you left off.
- Highlights, underlines and notes, with a side panel listing them. Notes can be exported to Markdown.

### Other screens

First-run setup (start a new library or import an export), import progress, review queue, tag manager, library export and import with a match report, and settings. Dark mode follows the system.

---

## Milestones

Each milestone ends with something you can run.

| # | Milestone | Done when |
|---|---|---|
| **M0** | Foundations and format test: install toolchains, set up the repository, CMake presets, CI, dependency licence check. Test sample files of every format with the engines. Build a trial AppImage and choose the glibc baseline | An empty Qt window builds on Windows, Arch and Ubuntu in CI, its AppImage starts on the other test distros, and there's a documented result for MOBI, AZW3 and FB2 |
| **M1** | Data core: schema with stable IDs, migrations, repositories, query builder, settings registry, 100k synthetic data generator | Filter queries meet the targets below in tests |
| **M2** | Import: worker process, scanning, hashing, duplicates, embedded metadata, covers, resumable jobs | A real folder of thousands of files imports, and bad files don't stop it |
| **M3** | Library UI: tabs, grid and list, filter sidebar, details and metadata editor, tags, search box | You can browse, filter, tag and edit your real collection |
| **M4** | Reader for PDF and EPUB: rendering, navigation, table of contents, in-document search, resume | Double-clicking a publication opens it, including large textbooks |
| **M5** | Other formats: DjVu, MOBI/AZW3, XPS, CBZ, FB2, using the M0 results | Every format renders and supplies a cover and, where it has any, text |
| **M6** | Metadata lookup: providers, all three modes, review queue, provenance and undo | Each mode behaves as specified, and offline mode makes no network requests |
| **M7** | Volumes: child articles, table-of-contents proposals, open at page, per-article tags | A journal volume can be split into searchable, citable articles |
| **M8** | Full-text search: indexing, snippets, jump to match, "no text" flag | A phrase search across the library opens the right page |
| **M9** | Highlights and notes: anchoring on fixed and reflowable formats, panel, Markdown export | Annotations survive restarts, file moves and EPUB font changes |
| **M10** | Citations: BibTeX, RIS, CSL-JSON export, and copying a formatted citation or reference list in Harvard, APA, IEEE or Chicago | Output imports cleanly into BibTeX and Zotero, and each style matches test fixtures for every publication type |
| **M11** | Library export and import: bundle format, content matching, merge and replace, settings transfer, match report | A library exported from one computer restores all tags, notes and settings on another, with its files in a completely different folder structure |
| **M12** | Release: first-run flow, Windows installer and portable zip, Linux AppImage, Flatpak and AUR package, performance and crash hardening, docs, making the repository public | Clean installs work on fresh Windows, Arch and Ubuntu machines, and the Linux packages pass the distro checks |

### Performance targets at 100,000 publications

These are goals to test against, not measurements yet.

- Filter or search result: under 100 ms.
- Library window open after launch: under 2 seconds.
- Smooth scrolling in both views.

### Main risks

| Risk | Plan |
|---|---|
| AZW3 files open but show nothing | MuPDF never receives KF8 files: Papertrail detects them from their contents and converts them first. Import also treats a blank result as a failure, not a success |
| libmobi is quiet (last push October 2024) | Pinned version, only ever run in the worker process, with all use confined to the engine adapter. We may need to maintain fixes ourselves |
| Annotation anchors break on EPUB when the layout changes | Store chapter and text offset rather than screen position. Addressed directly in M9 |
| Scanned files can't be identified automatically | They're flagged and filterable, with manual entry. OCR follows V1 |
| Wrong metadata matches | Provenance keeps earlier values, and any change can be undone |
| MuPDF API changes between versions | Pinned version, with all use confined to the engine adapter |
| Arch updates break the build, because it's a rolling distribution | The Arch CI job reports this early, and release builds are unaffected because they use a pinned base |
| "All distros" can't be fully tested | Arch and Ubuntu get full testing. Other distros get automated checks on a representative set, and the source build covers the rest |
| "Harvard" isn't one single standard, and universities vary | Default to the widely used Cite Them Right variant, and name it in the UI so users can check it against their university's guide. Full CSL styles on the roadmap cover other variants |
| Flatpak's sandbox limits file access | Request broad filesystem permission and explain why in the docs, since this is core to how Papertrail works |
| Unsigned Windows installer shows SmartScreen warnings | Accepted for now. A code-signing certificate is a paid option to decide on at M12 |
| The same paper exists as different files on two computers, so hashes don't match | Identifier matching proposes the link for review, and unmatched publications keep their tags until linked |
| Hashing is slow on a large collection during import | Runs as a resumable background job, using size to rule out files that can't match before hashing them |

---

## Project layout

```
Papertrail/
├─ CMakeLists.txt, CMakePresets.json, LICENSE (AGPL-3.0), README.md
├─ src/
│  ├─ core/       database, migrations, repositories, query builder, jobs
│  ├─ engine/     DocumentEngine interface, MuPDF, DjVu, MOBI adapters
│  ├─ import/     scanner, fingerprinting, extractors, classifier
│  ├─ metadata/   providers, matching, provenance
│  ├─ citation/   BibTeX, RIS, CSL-JSON, style formatters
│  ├─ transfer/   library export and import, file matching, merge rules
│  ├─ worker/     papertrail-worker executable
│  └─ ui/         library/, reader/, review/, settings/
├─ tests/         unit tests, fixtures, recorded API responses, sample files
├─ packaging/     Windows installer, AppImage, desktop file, icons
└─ .github/workflows/
```

---

## Verification

- **Automated, on Windows, Arch and Ubuntu in CI:** migrations, query builder, parsers, metadata matching (using recorded API responses), and citation output against expected files.
- **Linux coverage:**
  - **Arch and Ubuntu (full testing):** the whole test suite on a native build, plus the end-to-end pass below, on both Wayland and X11. The AppImage and Flatpak also get the same pass on both before a release.
  - **Other distros (automated checks):** the AppImage and Flatpak start up and pass a smoke test (open a library, render a PDF and an EPUB) in Debian, Fedora and openSUSE containers.
- **Your files stay untouched:** a test hashes a sample library before and after import, reading and annotating, and fails on any change.
- **Bad input:** corrupt, truncated and password-protected files must not bring down the app.
- **Paths:** non-ASCII names, long Windows paths, spaces, and offline or repointed roots.
- **Offline mode:** a test fails if any network request is made.
- **Export and import round trip:**
  - Export a library, shuffle and rename its files into a different folder structure, then import into an empty library. All tags, notes and settings must come back and every file must relink.
  - Run this from Windows to Linux and from Linux to Windows.
  - Also cover files that are missing or changed, importing the same export twice (no duplicates), merging into an existing library, and reading an export from an older version.
- **Performance:** the 100k synthetic library is checked against the targets above.
- **End-to-end pass at each milestone,** run in the real app:
  1. Import a mixed folder.
  2. Filter by tab, tag and year.
  3. Open each format.
  4. Highlight some text, restart the app, and confirm the highlight is still there.
  5. Search for a phrase across the library.
  6. Export citations.

---

## What I'll need from you

- **Approval to install** MSYS2, plus Arch and Ubuntu on WSL2, on this PC. WSL currently only has Docker Desktop's distribution. If you already have real Arch or Ubuntu machines, they would be better for the final desktop checks.
- **A GitHub account** for the private repository, before M0's CI step. I'll confirm with you before creating it.
- **Sample files**, ideally a real folder from your collection, plus MOBI, AZW3, FB2 and DjVu examples if you have them.
- **A name check before going public.** I recall SolarWinds having a log-management product called Papertrail, but I haven't verified this. Check for a trademark conflict before the public release.

## Roadmap after V1

OCR for scanned documents, Zotero/Mendeley/BibTeX import, automatic article detection in volumes without a table of contents, full CSL citation styles, collections and saved searches, a browser connector, and exporting annotated PDF copies.
