# M7 results

Tested on 8 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC. This was built overnight, without anyone trying it by hand.

A journal issue can now be split into articles, each searchable, taggable and able to be opened at its own first page. The same works for conference proceedings (papers) and books and textbooks (chapters). This document says "volume" for any of these.

## Changed after hand testing

- **Journal volumes are now journal issues.** A volume holds several issues, and a file is normally one issue, so the type is **Journal issue** and the tab is **Journal Issues**. It's still stored as `journal_volume`, because the schema's type check can't change without rebuilding the publications table, and nothing on screen shows the stored name. `type:issue` finds them in a search. Wording that meant any volume now says "publication", or names what it is, such as **Show the issue**.
- **Titles the contents only number are read from the articles' pages.** A real issue's contents listed only "01", "02" and so on. For any entry that's only a number (or "Article 3", "Chapter 7", a Roman numeral and the like), the dialog now reads the largest text on its first page, which is usually the title, and fills it in as it arrives, marked **Title from its first page**. A title typed over isn't replaced.
- **A DOI printed on an article's first page is kept,** when there's exactly one before any list of references, so a cited DOI isn't taken for the article's. With a DOI, a lookup can fill in its authors and the rest.
- **Articles made earlier with number titles are named, not repeated.** Choosing **From contents...** again offers them as **Names the article already made**, and confirming gives them their titles and DOIs, keeping anything else about them, such as tags and notes.

After a second round of hand testing:

- **The reader's contents show article titles.** Where the file's own contents only number an entry, and an article in the library starts on that page, the article's title is shown instead. The file isn't changed.
- **Each article has a cover of its own first page.** These are made in the background by the worker the first time an article is shown, and kept in the cache folder beside the issue's (named by the file's hash and the page). Until one is ready, the issue's cover stands in.
- **An article opened from the library shows only its own pages.** The page number counts from its first page, and its contents, thumbnails and search stay within it. **Whole issue** in the reader's toolbar shows the rest of the file, and pressing it again goes back. Following a match or a note to a page outside the article shows the whole issue too.
- **An article opened from its issue's list shows the whole issue,** at the article's first page, as before. Right-clicking an article there also offers **Open on its own**.
- **Issues of one journal can be told apart.** An issue's card shows its numbers and date, such as "Vol. 7, No. 11, Nov 2026", in place of the year, and the list adds them after the journal's name. **From contents...** reads them from the cover or a running header, such as "VOL. 7, NO. 11, NOVEMBER 2026", and fills them in where the issue and its articles have none. Nothing already filled in is changed, and a month is only added alongside its own year.
- **The reader's Contents panel wraps long titles** at the panel's width, with more space between entries, so a title can be read in full. Widening the panel rewraps them.

## How it works

- **From the table of contents:** select a volume and choose **From contents...** in the details panel, or **Create articles from contents...** on the right-click menu. Papertrail reads the file's contents and lists what it finds, with page ranges worked out. You tick what you want, correct anything wrong, and nothing is created until you confirm.
- **By hand:** **Add...** in the details panel, with a title, authors, DOI and pages.
- **Which level:** if the contents are nested, you choose the level, starting with the one with the most entries, since that's most likely to be the articles.
- **Unlikely entries:** covers, contents pages, indexes and the like are listed but not ticked. Anything already added is also left unticked.
- **Opening:** an article opens at its first page, in a tab of its own, and remembers its own place after that.
- **In the details panel:** a volume lists its articles in page order, and double-clicking one opens it. An article says what it's part of, with a button such as **Show the issue**.
- **What it inherits:** the journal name, volume, issue, year, publisher, ISSN and conference details come from the volume. A lookup or an edit can then fill in the rest.
- **Editing:** an article's first and last pages can be corrected in its editor.
- **Deleting:** **Delete article...** asks first, and leaves the volume and its file alone.

Articles are ordinary publications, so tags, notes, collections, search, filters and online lookups all work on them. Removing a volume's folder hides its articles too, and they come back with it.

## Things you may want to check

- **Page numbers are positions in the file,** not the numbers printed on the page, and the two usually differ. So they aren't put in the citation `pages` field. That stays empty until a lookup or you fill it in, which matters for M10.
- **Articles from a table of contents are marked as needing checking,** because the contents only give a title. A large volume can add many to **Needs checking**. They can be cleared together with **Mark as checked**.
- **Contents are only read from PDF, DjVu and XPS-style formats with fixed pages.** An EPUB's pages change with the window and text size, so a page range wouldn't mean anything, and the dialog says so.

## How it differs from the plan

- **"Expand to show their child articles"** is a list in the details panel, not rows that open up in the grid and list. Expandable rows don't fit the views' design for 100,000 publications, where only a few pages of rows are held at once.
- **Articles also appear as their own entries** under All and Journal Articles, alongside the issue under Journal Issues.

## Decisions made while you were away

- **Adding a volume to a collection adds only the volume,** as you chose.
- **A book or textbook can hold chapters,** as well as the journal volumes and proceedings in the plan, because the same steps work for them.

## How it's been tested

- **Automated:** all 13 test programs pass on all three platforms. The new `test_volumes` covers working out page ranges, nested contents, entries that lead nowhere, and articles sharing a page. It also runs the dialog against a real file and a real worker, a file with no contents, an EPUB, a volume with no file, and a crash while reading.
- **Your files aren't changed:** the test hashes the file before and after.
- **Not tested yet:**
  - **By hand in the real window,** including how the dialog and details panel look.
  - **A real journal volume.** The only sample is a small generated PDF, and real tables of contents will be messier. This is the most important thing to try.
  - **A volume with hundreds of articles,** for how the list and dialog cope.

## Known limits

- **A file with no table of contents** needs its articles added by hand. Detecting them automatically is on the roadmap.
- **An article's cover is made when it's first shown,** so scrolling through many new articles shows their issue's cover for a moment.
- **An article's pages are positions in the file.** If they're wrong, an article on its own shows the wrong pages until they're corrected in its editor.
- **Several articles open at once** each use their own reader process, like any other open document.
