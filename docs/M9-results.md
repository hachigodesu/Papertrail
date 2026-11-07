# M9 results

Tested on 8 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC. This was built overnight, without anyone trying it by hand.

You can now highlight and underline text and add notes in the reader. They're kept in Papertrail's database, and your documents are never changed.

## How it works

- **Making one:** select text, then use **Highlight** on the toolbar, or right-click for a choice of colours, **Underline** and **Add a note...**.
- **Changing one:** right-click it on the page for its note, its colour or **Delete highlight**. A coloured edge shows that it has a note.
- **The Notes tab:** the reader's side panel lists them with their page and note. Clicking one goes there, and right-clicking gives the same choices.
- **Export:** **Export to Markdown...** on the Notes tab saves the publication's details, with each quote and note under its page.
- **In the library:** the details panel says how many a publication has.
- **Across pages:** a selection over several pages is one annotation.

## How they last

- **Restarts and moves:** they're tied to the file's contents, so moving or renaming the file makes no difference.
- **Files with fixed pages (PDF, DjVu, XPS, CBZ):** positions on the page are stored directly.
- **Reflowable files (EPUB, AZW3, MOBI, FB2):** screen positions wouldn't last, because pages change with the text size. So the highlight stores the words, where in the chapter they were, and which repeat of them it was. They're looked for again whenever the layout changes.
- **If the words can't be found:** it stays in the list saying so, and nothing is lost.
- **Another copy or format:** a highlight made on another version of the file, or another format, is listed as coming from there. It isn't drawn, because it may not line up.
- **Articles:** a highlight made while reading an article belongs to that article, and also shows when reading the whole volume.
- **Combining publications:** highlights carry over.

## Changed after hand testing

- **The Colour menu ticks a highlight's current colour,** on the page and in the **Notes** list, and each colour has a swatch beside its name.

## Decisions made while you were away

- **A note is attached to selected text.** There are no free-floating notes on an empty part of a page, since the anchoring depends on text. The details panel's Notes box still covers notes on the whole publication.
- **There are four colours,** with no custom picker.
- **Export is from the reader,** so a publication has to be opened to export its notes. Exporting several at once isn't there yet.

## How it's been tested

- **Automated:** all 15 test programs pass on all three platforms. The new `test_annotations` covers saving, ordering, combining publications, Markdown, and the panel.
- **The goal from the plan:**
  - **Restarts:** the test closes and reopens the reader.
  - **File moves:** the folder is moved and the file renamed, and the highlight is still there.
  - **EPUB font changes:** using a real book, a bigger font moves the words to a later page, and the highlight follows. The test then selects at the highlight's new position to check it really is over those words.
- **Repeated words:** a highlight on the second "the" on a page is still on that one after reopening.
- **Your files aren't changed:** the test compares the file's hash before and after.
- **By eye:** a picture of a highlight and the Notes tab, using an offscreen window with a poor fallback font.
- **Not tested yet:**
  - **By hand in the real window,** including dragging to select, the right-click menus, and how highlights look in dark and sepia modes.
  - **Real papers and books,** where columns and odd spacing may make text harder to find again in reflowable files.
  - **A file with hundreds of highlights.**

## Known limits

- **A repeated short phrase in a reflowable book** can land on the wrong repeat after a big layout change, if the page it's found on has changed a lot. It's right when the layout hasn't changed, and the words are always the same words.
- **A long highlight in a reflowable book** that now crosses a page break only shows the part on its first page, starting from its first 60 characters.
- **Scanned pages have no text to select,** so they can't be highlighted until OCR arrives.
- **In dark mode,** highlight colours are drawn as they are over the darkened page.
- **M11's export** needs to include these. They have permanent IDs for this, and the plan already covers what to do when contents differ.
