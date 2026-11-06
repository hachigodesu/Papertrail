# M1 results

Measured on 7 November 2026 on the development PC, in Release builds, on a synthetic library of 100,000 publications (`tools/gen_library`). Timings are medians of 5 runs, and each search is timed as a first search. Run them with `ctest --preset windows-perf` or `ctest --preset linux-perf`.

The library is synthetic, so it shows how the design scales rather than how a real collection behaves. Its 60-word vocabulary makes "common word" searches a worst case, matching about half the library.

## Results list: first page and total

The plan's target is under 100 ms.

| Query | Windows | Arch | Ubuntu |
|---|---|---|---|
| Everything, any sort | 0.2 ms | 0.2 ms | 0.2 ms |
| Articles tab | 2.4 ms | 2.0 ms | 1.9 ms |
| Top-level tag and everything under it | 27.7 ms | 29.5 ms | 29.4 ms |
| Format: PDF (slowest filter) | 58.3 ms | 60.8 ms | 61.2 ms |
| Combined: tab, tag, year, status, language | 31.3 ms | 31.2 ms | 31.6 ms |
| Search: common word | 13.1 ms | 12.6 ms | 12.5 ms |
| Search: single-letter prefix | 34.9 ms | 35.0 ms | 36.1 ms |
| Search: common word, ranked by relevance | 60.6 ms | 66.8 ms | 69.5 ms |

Scrolling 90% of the way down the full list takes under 4 ms a page on all three.

## What's above or near a limit

- **Ranked search on a very common word.** Usually 60–70 ms, but one Windows run measured 128 ms, right after generating the library. The test allows 250 ms for ranked search instead of the plan's 100 ms, because it has to score every match and isn't the default sort.
- **The whole sidebar.** Computing every filter's counts takes 220–540 ms, and one Windows run with a cold cache took about 1.3 seconds. The plan sets no target for this. M3 must run it in the background, after the list appears, and cancel it when the filter changes.

## Other numbers

| | |
|---|---|
| Time to generate 100,000 publications | 53–63 s (about 1,700 a second) |
| Library database size | 319 MB (about 3.2 KB a publication), including the search index |

## Design decisions from testing

- **Sort indexes:** there's one index for each way of sorting, with and without a tab's type. Sorting by year went from 88 ms to 0.2 ms.
- **Search matches are reused:** a search's matches are worked out once, then reused by the page, the total and the sidebar until the text or the library changes. Common-word search went from 180 ms to 13 ms.
- **Pinned SQLite:** SQLite 3.54.1 is built from source on every platform, checked against its published hash.

## Not done yet

- Nothing in the app uses this yet. The library window arrives in M3.
- The `job` and `annotation` tables exist, but nothing uses them until M2 and M9.
- File fingerprints for publications whose file isn't found on import (M11) will need a later migration.
- The `query-timings` CI job only reports timings, because shared machines vary too much to fail a build on them.
