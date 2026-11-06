# M2 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC.

It's been tested on a real folder of 199 journal-article PDFs (0.57 GB), and at scale on 3,000 generated documents with subfolders, exact copies and bad files mixed in. M2's goal mentions a real folder of thousands, so the real-file test is smaller than that.

## Real-folder results

The folder's location isn't recorded anywhere: it was mapped to a temporary drive letter so the test database only held that letter, and the mapping and test library were removed afterwards. Example strings in the code and tests are made up.

| Result | |
|---|---|
| Imported | 199 of 199, in about 17 s (about 12 files a second) |
| Files changed | None, checked by hashing the folder before and after |
| Unreadable, or worker crashes | 0 |
| Titles matching the opening of the file name | 196 of 199 (all 142 from metadata, 54 of 57 from the first page) |
| Have a DOI | 169 (85%) |
| Have a year | 131 (66%) |
| Have authors | 118 (59%) |
| Marked for review | 66 (33%) |

The file-name comparison is only a proxy for accuracy. It works here because these files happen to be named after their titles, and it checks only the opening words.

### Types

The folder is meant to hold journal articles, which makes classification easy to check.

| | First run | After fixes |
|---|---|---|
| Journal article | 155 | 179 |
| Conference paper | 14 | 5 |
| Preprint | 8 | 7 |
| Other | 9 | 5 |
| Book or textbook | 5 | 2 |
| Technical report | 4 | 1 |
| Thesis | 4 | 0 |

Some of the remaining ones may well be correct: the preprints have arXiv stamps and no DOI, for example.

### What real files showed that generated ones didn't

- **Reference lists fool classifiers.** They mention theses, reports and conferences, so only the first page now decides a type firmly, and a DOI, ISBN or arXiv ID is only believed near the start.
- **Publishers' production codes end up as titles,** such as a code followed by a page range. These are now rejected.
- **Metadata contains HTML codes,** such as `&#x2014;`. These are now decoded.
- **The largest text is often a journal's masthead,** so short headings are skipped.
- **Titles wrap across text blocks,** so these are now joined.
- **One metadata entry can hold many authors,** so these are now split.
- **Some journals have "Proceedings" in their names,** so those no longer count as conference papers.

### Still weak until M6

Journal names aren't extracted at all, and 41% of files have no authors. Online lookup in M6 should fill most of these, since 85% of files have a DOI to look up. DOIs found in files haven't been checked against any outside source yet.

## What the import does

1. **Scan:** finds supported files and queues work for new or changed ones.
2. **Fingerprint:** hashes each file, and spots copies and moved files.
3. **Inspect:** a separate worker process reads the metadata and first pages, and writes a cover image.
4. **Create:** builds the publication from the best evidence, and marks it for review if that was a guess.

Work is held in a queue in the database, so stopping at any point and running again carries on.

| Evidence for the title, best first | Marked for review? |
|---|---|
| Embedded metadata (XMP, PDF Info, EPUB OPF, Kindle EXTH), unless obviously junk such as "Microsoft Word - draft3.docx" | Only if the type is a guess |
| The largest text on the first page | Yes |
| The file name | Yes |

## 3,000-file test

| | Windows | Arch | Ubuntu |
|---|---|---|---|
| Publications created | 2,587 | 2,587 | 2,587 |
| Exact copies linked | 413 | 413 | 413 |
| Unreadable, but listed | 29 | 29 | 29 |
| Covers written | 2,558 | 2,558 | 2,558 |
| Worker crashes recovered | 0 | **2** | 0 |
| Time for the rest after stopping at 800 jobs | 35 s | 31 s | 39 s |
| Time to rescan, with nothing to do | 0.3 s | 0.05 s | 0.05 s |

On all three, 383 MB of generated files stayed byte-for-byte identical, checked by hashing the whole folder before and after. That's about 85 files a second, but these files are tiny: the real folder ran at about 12. This test was run before the fixes that came from the real folder.

## A real crash, contained

On Arch, opening `tests/samples/truncated.pdf` crashes the process with `double free or corruption`, inside MuPDF's `pdf_repair_xref_aux`, before any of Papertrail's own code has touched the document.

- **What happened:** the worker died, the import retried once, then marked the file unreadable and listed it under its file name. Everything else imported normally, with the same results as the other platforms.
- **Cause not fully known:** it appears on Arch's MuPDF 1.28.5 package. Ubuntu's MuPDF, built from the same version's source, handles the file cleanly under strict malloc checks, and so does the MSYS2 package on Windows. Arch's build links system libraries where Ubuntu's uses MuPDF's bundled ones, which may be related.

### What this means for later milestones

- **M4 (reader):** the plan opens documents inside the app. A file like this would crash the whole app on Arch, so the reader must refuse files the worker couldn't get through, and may need to open documents in a separate process. M4 needs to settle this.
- **M12 (packaging):** the AUR package should consider building against the pinned MuPDF rather than Arch's.

## Changes from the plan

- **Worker communication uses standard input and output** rather than a local socket. It's simpler, and there's no socket for anything else to connect to.
- **Scanning uses one worker at a time.** Several at once would be faster on large collections, and can be added if a real collection needs it.
- **Watched folders aren't done yet.** Watching needs the running app, so it moves to M3. Rescans work now.
- **A password-protected or blank file is listed as unreadable** under its file name, so it isn't silently lost.
- **Online lookup isn't here yet** (M6), so metadata comes only from the files themselves.
- **AZW3 files get metadata but no cover or page count yet.** They need the conversion in M5.

## How to try it

There's no window yet, so `papertrail-import` runs the import from the command line:

```bash
papertrail-import --db library.db --cache cache-folder "path/to/your/papers"
```
