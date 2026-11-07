# M8 results

Tested on 8 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC. This was built overnight, without anyone trying it by hand.

You can now search inside documents across the whole library, see where the words were found, and open the right page.

## How it works

- **Searching:** tick **Inside documents** beside the search box. Publications whose text matches now appear along with those whose details match. It's off by default, and the choice is remembered.
- **Where it was found:** select a result, and **Found inside** in the details panel lists each page with the words around the match.
- **Going there:** double-click a place. The document opens at that page with your words searched for.
- **Searching while reading:** typing in the library's search box while a document is open goes back to the library, where the results are. (Added after hand testing.)
- **Articles:** a match inside a volume also finds the article whose pages it's on, and an article only lists matches from its own pages.
- **Indexing:** this happens by itself in the background once an import finishes, and when the app starts. The status bar shows how many are left, with **Stop indexing**. **File > Index document text** turns it off.
- **No text:** once a whole file has been read, its "No text" flag is corrected, replacing the guess made from the first few pages.

Searches use the same words as the ordinary search box. Field searches such as `author:knuth` can only be answered from a publication's details, so `fatigue author:knuth` finds "fatigue" inside documents by that author.

## Where the text is kept

The text goes in `fulltext.db`, beside the library and on this computer only. It's stored by the file's contents, so a moved file or a second copy isn't read again. It's rebuilt from your files when needed, and won't be part of M11's export. Text from files no longer in the library is forgotten the next time indexing runs.

**It will take up disk space,** roughly in line with the amount of text in your files. I've no figure yet from a real library, so that's worth checking.

## Decisions made while you were away

- **Indexing is on by default,** because the plan makes it part of importing and nothing leaves the computer. It starts on your existing library the first time you run this build, and you can stop it or turn it off.
- **Each page is searched on its own.** Several words must be on the same page, and words split across two pages aren't found together. This keeps results pointing at a specific page.
- **"Best match" order doesn't apply** to searches inside documents, and these sort by title. How well words match a page can't be fairly compared with how well they match a title.
- **A page that fails doesn't stop a file, and a file that fails doesn't stop the rest.** If the worker crashes or hangs on a file, what was read stays searchable, the problem is recorded, and that file isn't tried again unless its contents change.

## A problem testing found

Indexing made a Kindle reader test fail about 3 runs in 5 on Arch, and it could be reproduced on Windows. The reader and the indexer could both convert one AZW3 book at the same moment, using the same temporary file, so one of them failed and the reader showed an error. Each process now has its own temporary file, and neither replaces a copy the other has already finished. With the fix, the test passed all 5 repeats on Arch and all 3 on Windows, and there's a new test that makes the two collide on purpose.

## How it's been tested

- **Automated:** all 14 test programs pass on all three platforms. The new `test_fulltext` covers searching, phrases, accents, field searches, articles, hidden publications, duplicates, and search text that tries to break the query. It uses the real import and worker on PDF, DjVu, EPUB, AZW3, FB2 and XPS samples, along with a scanned file, a truncated file, a crash, a hang and stopping part-way.
- **Your files aren't changed:** the tests hash the folder before and after.
- **The goal from the plan:** a test searches for a phrase across a library, then opens the right page in the reader, with the words in its search box.
- **Speed, on Windows with a release build:** with about 4,100 documents of 20 pages each, or 83,000 pages:

| Search | Time |
|---|---|
| A word that's on every page | 222 ms |
| An exact phrase | 248 ms |
| A word that's nowhere | 39 ms |

These run in the background, so the window isn't held up. The time grows with how many pages contain the words, so a common word across a library 20 times this size could take several seconds. That size hasn't been timed.

- **Not tested yet:**
  - **By hand in the real window,** including how **Found inside** looks.
  - **Your real library.** Real papers have columns and odd spacing, so check that phrases are found as you'd expect. How long indexing takes and how large `fulltext.db` gets also need measuring. In the test above, filling the index took about 19 seconds, but that doesn't include reading the files.
  - **A very large library,** at the size just mentioned.

## Known limits

- **In EPUB and other reflowable formats,** the page number is only approximate, because pages move with the layout. Opening one goes to the first match in the file instead of the listed page.
- **Two-column papers** are read in whatever order the file gives, so a phrase crossing a column break may not match.
- **Scanned documents** have nothing to search until OCR arrives.
- **Words split with a hyphen at a line end** are joined only if the next line starts in lower case, so that names stay as they are.
- **The list refreshes when indexing finishes,** so a search during indexing only finds what's been read so far.
- **The worker reads at full speed,** so indexing a large library may be noticeable until it's done. It can be stopped at any time.
