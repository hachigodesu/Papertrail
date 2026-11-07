# M6 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC.

Papertrail can now fill in a publication's details from online sources, in any of the three planned modes, and any lookup can be undone. **Nothing is sent anywhere until you choose an online mode.** A library starts offline, and the first time you add a folder you're asked which mode you want.

## The three modes

You choose a default under **File > Online lookups...**, and can override it each time you add a folder.

| What was found | Online, with review | Online, no review | Offline |
|---|---|---|---|
| An identifier in the file, and the title fits | Applied | Applied | Never asked |
| An identifier in the file, but the title disagrees | Waits for review | Applied, and marked for checking | Never asked |
| A title search | Waits for review | Best match applied, and marked for checking | Never asked |

The title check is there because a file's DOI may belong to a paper it cites. A disagreeing title is also checked against the file name, since embedded titles are often junk.

Going offline also clears any lookups still waiting.

## What changed

- **Looking up by hand:** right-click publications and choose **Look up details online**, or use **File > Look up details for the whole library...**.
- **The review tab:** a **To review: N** button appears when something is waiting. It opens a side-by-side comparison, with what would change in bold, and **Use these details** or **Not a match**.
- **The pop-up:** when an import and its lookups have finished with something new to check, a box offers **Review now** or **Later**. It doesn't block the window, and it waits if you're reading. Choosing **Later** leaves it to the toolbar button until there's more to check. Stopping lookups doesn't bring it up. A large import only shows it at the end, so the button is how to start early.
- **Undo:** the details panel says which source supplied the details, with an **Undo** button. Undo puts back what that lookup changed, and leaves anything you've edited since.
- **Stopping:** the status bar shows progress and a **Stop lookups** button. Whatever's left carries on next time, including after closing the app mid-lookup.

### What a lookup won't touch

- **Fields you've edited by hand.**
- **An existing DOI, ISBN or abstract.** These only fill gaps.
- **Preprints.** A source can never move anything into or out of Preprints. If one says a work is a preprint, the details panel mentions it, and the checkbox stays yours to tick.
- **A more exact type.** A source saying "book" won't change your textbook, and the type only changes between tab groups.

## What's sent, and to whom

| Source | Used for | Fastest pace Papertrail uses |
|---|---|---|
| Crossref | DOI, then title search | 4 a second for DOIs, under 1 a second for searches |
| OpenAlex | DOI, then title search | 5 a second, within its daily allowance |
| arXiv | arXiv number | One every 3 seconds, as they ask |
| Open Library | ISBN | 1 a second |

Papertrail sends a DOI, ISBN, arXiv number or title. It never sends authors, notes, tags or anything from inside a file. A contact email is optional, is only sent to Crossref and OpenAlex, and only if you enter one. Answers are cached for 90 days, or 7 days for "not found".

## What the real services showed

- **OpenAlex's anonymous search allowance is small.** A DOI lookup costs nothing, but a search costs $0.001 against a $0.10 daily limit, so about **100 searches a day**. Papertrail only searches OpenAlex if Crossref has no good match, and when the allowance runs out it carries on with the other sources.
- **Crossref's searches are limited to 1 a second,** a fifth of its limit for single works, so they're paced separately.
- **Crossref's search can rank poorly.** Searching for one well-known paper returned chapters with similar names first, while OpenAlex found it. This is why both are asked.

## Problems found while testing

- **"Not found" was never remembered.** An empty answer was stored as a database null, which the table refuses, and the error was swallowed because a cache failure is meant to be harmless. A test of the cache caught it.
- **A lookup in progress when the app closed would have stayed stuck.** Starting an import also reset lookups that were really still running. Lookups now recover their own jobs when they start, and imports leave them alone.
- **A rate limit from one source stalled the whole queue.** With a daily allowance, that could have meant waiting for hours. Each source is now blocked separately, and the queue only waits when no source can answer.
- **The review table had Authors at the bottom, and oversized rows.** I only noticed this from a picture of the screen. Authors now follow the title, and rows fit their text.

### The Linux build hang

The first Linux run hung for over 30 minutes. The WSL virtual machine stopped answering, and used about 12 of the 16 logical CPUs with no I/O. It happened with both distros building at once, at their default of about 18 compile jobs each, in one VM with 7.9 GB of memory.

After restarting WSL, each distro ran one at a time with 8 jobs, and finished in about 3 minutes including the tests, with no hang. Memory use peaked at about 4.2 GB, sampled every 20 seconds. Running out of memory is the likely cause, but this wasn't confirmed. The VM's log couldn't be read while it was hung, and the hung run may have left part of the build done, so the rerun was a lighter load in more than one way. `CLAUDE.md` now says to build on one distro at a time with `-j 8`.

## Removing a folder

This came up while you were trying M6: publications stayed in the library after their folder was removed. The old version deleted the folder's file records, which left the publications showing as "No file linked", with nothing to reconnect them if the folder came back.

Removing a folder now asks what to do with the publications that are only there. Cancel is the default.

| Choice | What happens |
|---|---|
| **Remove, keep details** | They leave the library, and every filter, count and search. Their tags, notes and reading places are kept |
| **Remove, delete details** | They're deleted from Papertrail for good. Your files aren't touched |

- **Coming back:** adding the same folder again brings back the same publications, tags and all. If the files have moved, adding the new folder reconnects them by their contents.
- **Also in another folder:** a publication with a copy elsewhere stays, and only loses this location.
- **Deleting later:** **Library folders** shows how many are being kept, with a **Delete their details...** button.
- **Waiting work:** queued imports and lookups for that folder are dropped, and anything waiting for review comes back only with the folder. Nothing from a removed folder is looked up online.
- **Open items:** its publications' reader tabs close, and the details panel clears.

**Publications left by the old version can't reconnect.** Their file records were already deleted, so there's nothing to match them with. When you open your library, they'll be hidden. If you add that folder again, its files import as new publications. **Delete their details...** clears the old ones, which nothing in the library can reach any more.

Deleting details leaves covers and converted books in the cache folder, as nothing cleans that up yet. M11's export needs to decide what happens to hidden publications.

## How it's been tested

- **Automated:** all 12 test programs pass on all three platforms. The new `test_metadata` uses recorded answers in each source's real format, with made-up contents.
- **Offline stays offline:** with the app's real network code pointed at a server inside the test, adding a folder and asking for a lookup makes no connections, either before a mode is chosen or in offline mode. The same steps in an online mode do connect, which shows the test can fail.
- **Live, using public records:** Crossref, arXiv and Open Library were reached over secure connections on all three platforms, using `papertrail-lookup`. OpenAlex was checked on all three too. These tests don't run in CI, which never contacts the real services.
- **AppImage:** it builds on Ubuntu, starts, and now fails to build if the secure-connection plugin is missing.
- **By eye:** the review tab and the mode dialog, using offscreen pictures with a poor fallback font.
- **Not tested yet:**
  - **Your real library.** This sends your files' identifiers and titles to the sources, so it needs you to choose an online mode. Real-world match quality, and how many matches end up in review, are still unknown.
  - **By hand in the real window,** on Windows or on a Linux desktop.
  - **A live lookup from inside the AppImage.** Its plugin was checked, but `papertrail-lookup` isn't bundled.
  - **A real rate limit from a source.** Those paths use recorded answers.
  - **A very large lookup run.** At these paces, 10,000 publications would take from under an hour to several hours, depending on how many need a search.

## Known limits

- **The AppImage uses the system's OpenSSL 3.** It isn't bundled, so on a distro without it, lookups report that they can't reach the sources. The rest of the app still works.
- **A large library without identifiers will use up OpenAlex's allowance quickly.** Files searched after that only get Crossref's answer, and aren't searched on OpenAlex again automatically. Entering a contact email may help, but I haven't measured this.
- **A search that found nothing is remembered for 90 days,** even if you ask again by hand. Asking again by hand does retry a DOI, ISBN or arXiv number that wasn't found.
- **Lookups don't take details from a publication's other formats.** Each publication is looked up once, using its own identifiers and title.
- **Candidates can go stale.** One waiting for review stays there until you decide, even if you've since edited the publication.
