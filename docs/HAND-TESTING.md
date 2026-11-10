# Testing M12 by hand

M12 makes Papertrail into packages you can install. This list covers Windows. [LINUX-CHECKLIST.md](LINUX-CHECKLIST.md) covers a Linux machine.

**Done:** you went through this list on Windows, and the AUR package on Arch, before releasing 1.0.0. It's kept for trying later releases the same way, with the packages from the release's page rather than `build/package-output`.

## Before you start

Close Papertrail if it's open. The packages are in `build/package-output`:

- `Papertrail-1.0.0-setup.exe`
- `Papertrail-1.0.0-portable.zip`

The installed copy opens the same library as the one you've been testing with, in `%APPDATA%\Papertrail`, so nothing needs moving. It doesn't change the library's layout, so the build you've been using can still open it afterwards.

## 1. Installing just for you

1. Open `Papertrail-1.0.0-setup.exe`. Windows will probably say "Windows protected your PC", since it isn't signed: choose **More info**, then **Run anyway**. Note whether it does.
2. Read each page: the welcome, the licence (which you don't have to accept), and the choice of **Only for me** or **Anyone who uses this computer**. Choose **Only for me**. It shouldn't ask for administrator rights.
3. Leave the folder as suggested, tick **Desktop shortcut** if you'd like one, and install.
4. Leave **Open Papertrail** ticked and finish. Your library should open as usual.
5. Check the icon in the taskbar, the Start menu entry, and **Help > About Papertrail**, which should say 1.0.0, link to the source, and open a folder of licences with **Show licences**.
6. Open a few documents of different kinds, and look something up online if you use that.

## 2. Uninstalling

1. Open **Settings > Apps**, find Papertrail, and uninstall it. Read what it says will be kept.
2. Check the Start menu entry has gone, and that `%LOCALAPPDATA%\Programs\Papertrail` is gone or empty.
3. Check your library is still there by opening it with the build you've been using (`tools/windows/run-debug.cmd`).

## 3. Installing for everyone

1. Run the installer again and choose **Anyone who uses this computer**. It should ask for administrator rights, and suggest `C:\Program Files\Papertrail`.
2. Check it opens your library as before.
3. Uninstall it again from **Settings > Apps**.

## 4. The portable zip

1. Unzip it somewhere, such as your desktop or a USB stick, and open `papertrail.exe` inside.
2. It should start with the welcome page and an empty library, since it keeps its own in `Papertrail library` beside it. The welcome page says so.
3. Add a small folder, close it, open it again, and check it remembered. Then delete the unzipped folder.

## 5. The welcome page

With the portable copy, before adding anything, check the welcome page reads well. Its **Import a library from another computer...** button should ask for a `.papertrail` file.

## 6. Several notes at once

In the build you've been using, open a publication with a few highlights, and open **Notes** in the reader's side panel. Choose several with Ctrl-click, Shift-click or Ctrl+A, then right-click them: change their colour, copy them and paste somewhere, export them, and delete them. Deleting several asks first.

## Things I decided that you may want to change

- **The icon is still the placeholder** stack of pages from M0. A designed one can replace it: `packaging/make_icons.py` draws the Windows and window sizes from simple shapes, so a new design would need an SVG and new PNG and ICO files.
- **The installer's licence page** shows the AGPL with a **Next** button, not "I agree", since free software licences don't need accepting.
- **The portable copy keeps its library beside itself,** which is what "portable" usually means, so it doesn't share your installed library.
- **Uninstalling keeps your library,** notes, highlights and backups, as most apps do. Delete `%APPDATA%\Papertrail` by hand to remove them.
- **Deleting one highlight from the Notes list still goes straight away,** as before, and only several are asked about.

## When it looks right

Tell me, along with what you found on Linux, and I'll commit M12. Pushing a `v1.0.0` tag then makes a draft release on GitHub with every package, which stays private until you publish it.

## For 1.0.2

What its tests can't vouch for. The packages are on the release's page.

**Reader**

- **Contents:** an issue whose contents number some entries "1", "2", "3" shows only the titled ones. A document whose contents are only numbers still shows them.
- **Highlighting:** the toolbar has no Highlight button. Right-clicking selected text highlights in each colour, underlines and adds a note.
- **Dark pages:** real papers' figures and photos keep their colours, charts drawn as lines are still inverted, scanned PDFs still go dark, and an EPUB's pictures keep theirs. Scrolling a long, figure-heavy PDF in dark mode isn't noticeably slower.

**Library**

- **Articles in the details:** long titles wrap, and wrap again when the panel is resized. Double-clicking and the right-click menu still work.
- **Show its articles:** right-clicking an issue lists its articles in page order, clearing the tab and sidebar filters. Two issues whose titles overlap, such as "Vol 5" and "Vol 5 Issue 2", both show.
- **Saved searches:** saving from the sidebar and from the search box's **?** menu, showing one, replacing, renaming and deleting it, and one with an author or journal that isn't among the 50 listed.
- **Retracted:** a retracted paper with a cover has the red bar along the bottom of its preview in the grid, the list and the details, and stacks with "File missing".

**Settings (Ctrl+,)**

- **Theme:** switching Light, Dark and Same as the system with a document and a dialog open leaves nothing in the old colours, and the choice is kept after a restart. With Same as the system, changing Windows' own mode while Papertrail is open follows it.
- **Reading:** page layout, colours and text size apply to a document opened for the first time, and the text size to an EPUB.
- **Switches and buttons** each take effect or open their dialog.

**Elsewhere**

- **Upgrading:** opening a library from 1.0.1 adds saved searches to it, after a backup in its `backups` folder. 1.0.1 can't open it afterwards.
- **Windows 11's style,** which draws selected rows itself, in the Notes and Articles lists.
- **Theme switching on a Linux desktop,** KDE or GNOME on Wayland.

## For 1.1.0

What its tests can't vouch for. The packages are on the release's page.

**Reader**

- **Smart Jump:** in real papers, Ctrl+clicking figures, tables, equations, sections and numbered and author-year citations previews the right thing, and the preview fits on the screen near the click. The pointing hand shows while Ctrl is held. A reference to something the paper doesn't have says so in the status bar. A large book answers within a couple of seconds.
- **Contents (Ctrl+G):** the popup appears over the page, starts at the section being read, and finds entries in a long book's contents as you type. Escape and a click elsewhere close it.
- **Side by side:** opening beside from the library and from a tab's menu, the same document twice, closing either side with its ×, and Separate. Ctrl+F and Ctrl+G go to the side last clicked. Both sides scroll smoothly, and memory stays reasonable with two long documents.
- **Areas:** Shift and a drag, and **Mark an area**, on a PDF and a DjVu file. The outline shows in each page colour, the Notes panel's picture is a sensible size, **Copy picture** pastes into a word processor, and a marked copy opened in another PDF reader shows a rectangle. An EPUB says areas need fixed pages.

**Library**

- **Retractions:** with lookups on, a new retraction brings up the message once, while you're in the library rather than reading. The status bar's count stays after a restart, and clicking it shows them.
- **Case law:** a real judgment from the UK and one from the US are recognised on import and marked for review, with sensible names, courts and citations. Switching **Cite as** in the editor changes the copied reference. A summary from Claude of each reads as a brief.
- **Import references:** a real Zotero, Mendeley or EndNote export, with accents and attached files. The preview's counts look right, and the import changes what you'd expect. Undo from the details panel takes an entry's changes back. Folders outside the library are named in the preview.

**Elsewhere**

- **Upgrading:** opening a library from 1.0.2 adds case law to it, after a backup in its `backups` folder, keeping every publication, file, tag, collection, highlight and note. 1.0.2 can't open it afterwards.
- **The release:** the tag's workflow takes each package from the commit's own CI runs rather than building them again.

## For 1.2.0

What its tests can't vouch for. The packages are on the release's page.

**Reader**

- **Smart Jump from contents:** in a real book whose contents have no links, Ctrl+click a chapter, a section and an appendix in the contents. Each preview shows its heading, not the contents again, and Go there goes to it. In a book whose printed page 1 comes well after the cover, it still finds the right chapter.
- **Smart Jump to the thing itself:** a mention of an appendix or section far from it (such as "Appendix E" on page 8 of a 200-page book) previews the appendix, not its line in the contents. Figures and tables listed in a list of figures still go to their captions. A section's name repeated at the top of each page doesn't win over its heading.
- **Bookmarks:** Ctrl+B, the page's right-click menu and the side panel's button each add one. Renaming with F2 or a slow second click, deleting with Delete, and going there by double-click. They're listed under Bookmarks in Ctrl+G. In an EPUB, a bookmark still goes to the same words after changing the text size.
- **Full screen:** F11 from a document and from two side by side. Escape leaves, but first clears a selection or stops drawing an area. Ctrl+F, Ctrl+G and Ctrl+W still work while it's on. Going to the library tab leaves it. The window comes back maximized if it was.
- **Turning pages:** Ctrl+R and Ctrl+Shift+R on a PDF and a DjVu file, in each layout. Highlights, selections, search matches, areas and links all stay on the right words; drawing an area and Ctrl+clicking work on a turned page; thumbnails and Smart Jump previews are turned too. The document opens turned the same way next time. An EPUB says it can't be turned.
- **Forward:** after following a link or contents entry and going back, Alt+Right, the → button and a mouse's forward button go forward again.

**Library**

- **Related papers:** with lookups online, a paper with a DOI shows its references and citing papers. Those in your library are marked in bold, double-click shows them, and Add to library adds others without a file. A paper without a DOI, and lookups offline, each say why there's nothing. Refresh asks again.
- **Reading at a glance:** right-click the list's header to show Rating, Last opened and Progress. Sorting by each from the Sort box and by clicking the header. The Rating filter, and Opened in the last 30 days under Read status, which sorts newest first. Progress follows reading a document further, and a column layout from 1.1 is kept.
- **Show its articles:** an issue whose title is inside another's shows only its own articles, with a chip by the search box that clears it. Saving the search and showing it again shows the same issue's.

**Elsewhere**

- **Upgrading:** opening a library from 1.1.0 adds bookmarks and related papers to it, after a backup in its `backups` folder. 1.1.0 can't open it afterwards.
- **Exporting** a library with bookmarks and importing it on another computer brings them.

## For 1.3.0

What its tests can't vouch for. The packages are on the release's page.

- **A real scanned book or paper,** in PDF and in DjVu: after indexing, the status bar says it's recognising scanned text, page by page, and Stop stops it. Closing and opening Papertrail carries on where it stopped. Afterwards, a search with **Inside documents** finds words from it, and the **No text** filter still lists it.
- **Reading a scanned page** that hasn't been recognised yet: within a few seconds the status bar says its text was recognised, and then it can be searched with Ctrl+F, selected, copied, highlighted, and Ctrl+clicked for Smart Jump. Its highlights are still there next time.
- **An old paper with a poor text layer of its own,** where Ctrl+F misses words that are on the page: **Recognise this document's text** in the reader's right-click menu recognises the page being read within a few seconds, and then Ctrl+F finds the words. The rest follow in the background, and a library search with **Inside documents** finds them afterwards. **Recognise its text** in the library's right-click menu does the same without opening it. The **No text** filter doesn't list it. With recognising turned off in Settings, the status bar says so.
- **A document that's partly scanned,** with some pages of text: only the scanned pages are recognised, as they're read.
- **Settings > Scanned documents:** turning it off stops it, and on again carries on. Choosing a language that isn't here asks before fetching it, says how large it is, and the language is ticked once it's arrived. With lookups offline it says it can't. Choosing another language recognises scanned documents again with it.
- **How busy the computer is** while a large scanned book is recognised: the window stays responsive, and other programs aren't slowed much.
- **Packages:** the installer, the portable zip, the AppImage and the Flatpak each recognise a scanned page, and their self-test says so. The licences folder has Tesseract's, Leptonica's and the English model's.
