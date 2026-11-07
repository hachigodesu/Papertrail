# M7 results

Tested on 8 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC. This was built overnight, without anyone trying it by hand.

A journal volume can now be split into articles, each searchable, taggable and able to be opened at its own first page. The same works for conference proceedings (papers) and books and textbooks (chapters).

## How it works

- **From the table of contents:** select a volume and choose **From contents...** in the details panel, or **Create articles from contents...** on the right-click menu. Papertrail reads the file's contents and lists what it finds, with page ranges worked out. You tick what you want, correct anything wrong, and nothing is created until you confirm.
- **By hand:** **Add...** in the details panel, with a title, authors, DOI and pages.
- **Which level:** if the contents are nested, you choose the level, starting with the one with the most entries, since that's most likely to be the articles.
- **Unlikely entries:** covers, contents pages, indexes and the like are listed but not ticked. Anything already added is also left unticked.
- **Opening:** an article opens at its first page, in a tab of its own, and remembers its own place after that.
- **In the details panel:** a volume lists its articles in page order, and double-clicking one opens it. An article says which volume it's part of, with **Show its volume**.
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
- **Articles also appear as their own entries** under All and Journal Articles, alongside the volume under Journal Volumes.

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
- **An article shows its volume's cover,** not its own first page.
- **An article opens the whole volume's file,** at its first page. Nothing stops you reading past its last page.
- **Several articles open at once** each use their own reader process, like any other open document.
