Papertrail 1.6.0, a library manager and reader for academic publications, for Windows and Linux.

## What's new

- **CSL citation styles.** Copy references and citations in the styles Zotero and Mendeley use, as well as the four Papertrail has always had:
  - 35 come with Papertrail, among them Vancouver, Nature, AMA, MLA, Chicago notes and bibliography, ACS, IEEE's CSL style, BMJ, The Lancet and OSCOLA.
  - **Cite > Style > More styles...** lists them all, grouped and searchable, with a preview of what you've chosen. The five you used last are offered in the Style menu.
  - **Add a style file...** adds any other `.csl` file, such as one from the Zotero Style Repository, to your library. It's tried before it's kept, and it goes with your library when you export it.
  - **File > Settings... > Citations** chooses the style and the language CSL styles are written in.
  - A note style's citation is its note, so the menu says **Copy note**. Court cases keep their Bluebook or OSCOLA form in every style.
  - Nothing is fetched online for them, and a style can't reach your files or the network.
- **Look up a selection.** Choose some words in a document, then **Look up** from the right-click menu, or press Ctrl+Shift+L. The side panel's new Look up page lists where else your library mentions them: the document you're reading first, then other papers' pages, highlights and notes, and details. **Search the library for these words** carries on in the library.

  With lookups online, it can also look on Wikipedia, in your system's language, and in Wiktionary. Only the words are sent, and only when you click, unless you tick **Always look on Wikipedia and Wiktionary too**.
- **A PDF copy of a book.** **Save a PDF copy...**, in the library's right-click menu and the reader's page menu, makes a PDF of an EPUB, Kindle book, FB2, XPS, CBZ or DjVu, for printing or another app:
  - A book is laid out at the page size, text size and margins you choose, keeping its text, links and contents.
  - A DjVu's pages keep its words, including words Papertrail has recognised, so the copy can still be searched.
  - The book itself is never changed, and stopping a copy leaves nothing half-written behind.
- **Datasheets & Manuals.** Datasheets, user manuals and reference manuals have a tab of their own, with the products they cover, which can be searched for. They're cited under their manufacturer, with their document number and revision, and exported as BibTeX's `@manual`. A datasheet or manual added to your folders is recognised from its first page, for you to check.

## What's changed

- **Standards are now Standards & Guidelines,** and guidelines and codes of practice with a number are recognised as standards.
- **Bookmarks are easier to read,** with a bookmark's page beneath the name you gave it.
- **A library opened in 1.6.0 can't be opened in 1.5.0 again,** because 1.6.0 adds datasheets and manuals and keeps the styles you add. Papertrail makes a backup before upgrading your library.

## Downloads

| File | For |
|---|---|
| `Papertrail-1.6.0-setup.exe` | Windows 10 and 11. Installs just for you, or for everyone |
| `Papertrail-1.6.0-portable.zip` | Windows 10 and 11, from any folder or a USB stick, keeping its library beside it |
| `Papertrail-1.6.0-x86_64.AppImage` | Most Linux distros, from Ubuntu 22.04 and Debian 12 on |
| `Papertrail-1.6.0.flatpak` | Any Linux distro with Flatpak. Installs the KDE runtime from Flathub |
| `SHA256SUMS` | Checking a download is the one published |

The Windows downloads aren't code-signed, so Windows may say "Windows protected your PC" the first time. Choose **More info**, then **Run anyway**.

See the [README](https://github.com/hachigodesu/Papertrail#readme) for how to install it and what it sends online.
