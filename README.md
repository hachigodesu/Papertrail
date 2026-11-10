# Papertrail

[![Windows](https://github.com/hachigodesu/Papertrail/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/windows.yml)
[![Arch](https://github.com/hachigodesu/Papertrail/actions/workflows/arch.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/arch.yml)
[![Ubuntu](https://github.com/hachigodesu/Papertrail/actions/workflows/ubuntu.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/ubuntu.yml)
[![AppImage](https://github.com/hachigodesu/Papertrail/actions/workflows/appimage.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/appimage.yml)
[![Flatpak](https://github.com/hachigodesu/Papertrail/actions/workflows/flatpak.yml/badge.svg?branch=main)](https://github.com/hachigodesu/Papertrail/actions/workflows/flatpak.yml)

Papertrail is a library manager and reader for university students and researchers, for Windows and Linux. It indexes your journal articles, textbooks, theses, reports and standards **where they already are**, keeps their details, tags and notes in a library of its own, and opens them in a built-in reader.

**It never changes your files,** and it stays offline until you choose otherwise.

- [Installing](#installing)
  - [Windows: the installer](#windows-the-installer)
  - [Windows: the portable zip](#windows-the-portable-zip)
  - [Linux: which package to choose](#linux-which-package-to-choose)
  - [Linux: the AppImage](#linux-the-appimage)
  - [Linux: the Flatpak](#linux-the-flatpak)
  - [Arch Linux](#arch-linux)
  - [Checking a download](#checking-a-download)
- [Getting started](#getting-started)
- [Features](#features)
- [Where your library is kept](#where-your-library-is-kept)
- [Your files and your privacy](#your-files-and-your-privacy)
- [If something goes wrong](#if-something-goes-wrong)
- [Building from source](#building-from-source)
- [Reporting problems](#reporting-problems)
- [Licence](#licence)

## Installing

Every download is on the [releases page](https://github.com/hachigodesu/Papertrail/releases). Under the newest release, open **Assets** to see the files.

| You use | Download | Then follow |
|---|---|---|
| Windows 10 or 11 | `Papertrail-<version>-setup.exe` | [The installer](#windows-the-installer) |
| Windows, from a USB stick or without installing | `Papertrail-<version>-portable.zip` | [The portable zip](#windows-the-portable-zip) |
| Ubuntu, Debian, Fedora, Mint, openSUSE and most other Linux | `Papertrail-<version>-x86_64.AppImage` | [The AppImage](#linux-the-appimage) |
| Fedora Silverblue, or any Linux with Flatpak | `Papertrail-<version>.flatpak` | [The Flatpak](#linux-the-flatpak) |
| Arch Linux | The source | [Arch Linux](#arch-linux) |

Papertrail needs a 64-bit computer. Your library is kept separately from the program, so updating or removing Papertrail never touches it.

### Windows: the installer

**Installing**

1. Download `Papertrail-<version>-setup.exe` from the releases page.
2. Open it from your Downloads folder.
3. Windows may say **"Windows protected your PC"**. This is because the installer isn't signed with a paid certificate, not because anything is wrong with it. Choose **More info**, then **Run anyway**.
4. Choose who to install it for:
   - **Only for me** (suggested): installs into your own user folder, and needs no administrator password.
   - **Anyone who uses this computer**: installs into Program Files, and asks for an administrator password.
5. Leave the folder as suggested, and tick **Desktop shortcut** if you'd like one.
6. Choose **Install**, then **Finish**. Papertrail opens, and from then on it's in the Start menu.

**Updating:** download the new installer and run it the same way. Your library, tags and notes stay as they are.

**Removing:** open **Settings > Apps > Installed apps**, find **Papertrail**, and choose **Uninstall**. This removes the program only. Your library is kept, in case you install it again: see [where your library is kept](#where-your-library-is-kept) to remove that too.

### Windows: the portable zip

The portable copy runs from any folder without installing, and keeps its library in a **Papertrail library** folder beside it. It doesn't share a library with an installed copy.

1. Download `Papertrail-<version>-portable.zip`.
2. Right-click it and choose **Extract All...**. Choose where to put it, such as your Documents folder or a USB stick.
3. Open the extracted folder, then open `papertrail.exe`. If Windows says **"Windows protected your PC"**, choose **More info**, then **Run anyway**.

**Updating:** extract the new zip, then move the **Papertrail library** folder from the old copy into the new one before opening it.

**Removing:** delete the folder. That deletes its library too, so keep the **Papertrail library** folder if you want it.

### Linux: which package to choose

- **The AppImage** is one file that runs on most distros from Ubuntu 22.04, Debian 12 and Fedora 36 on, with nothing to install.
- **The Flatpak** suits any distro with Flatpak, including immutable ones such as Fedora Silverblue, and older distros the AppImage doesn't run on. It can read all your files, so it can index them wherever they are, but it can't change them.

### Linux: the AppImage

**Installing**

1. Download `Papertrail-<version>-x86_64.AppImage`.
2. Move it somewhere permanent, such as an `Applications` folder in your home folder.
3. Make it executable. In most file managers, right-click it, choose **Properties**, and tick **Allow executing file as program** (or **Is executable**). Or in a terminal:

   ```bash
   chmod +x ~/Applications/Papertrail-*-x86_64.AppImage
   ```

4. Double-click it to open Papertrail.

To add it to your app menu, use your desktop's own tools or an AppImage manager such as Gear Lever.

**Updating:** download the new AppImage, make it executable, and delete the old one.

**Removing:** delete the file.

### Linux: the Flatpak

**Installing**

1. If your distro doesn't have Flatpak yet, follow the [Flatpak setup guide](https://flatpak.org/setup/) for it.
2. Add Flathub, where the Flatpak gets the KDE libraries it runs on, if you haven't already:

   ```bash
   flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
   ```

3. Download `Papertrail-<version>.flatpak`, then install it from the folder you downloaded it to. The first time, this also downloads the KDE runtime, which is a few hundred megabytes:

   ```bash
   flatpak install --user Papertrail-*.flatpak
   ```

4. Open **Papertrail** from your app menu. You may need to log out and back in the first time for it to appear. You can also start it from a terminal:

   ```bash
   flatpak run io.github.hachigodesu.Papertrail
   ```

**Updating:** download the new `.flatpak` file and install it the same way.

**Removing:**

```bash
flatpak uninstall --user io.github.hachigodesu.Papertrail
```

This keeps your library. To remove that too, delete `~/.var/app/io.github.hachigodesu.Papertrail`.

### Arch Linux

The repository includes a `PKGBUILD` that builds Papertrail with Arch's own Qt, MuPDF and DjVuLibre:

```bash
git clone https://github.com/hachigodesu/Papertrail.git
```

```bash
cd Papertrail/packaging/arch && makepkg -si
```

`makepkg` checks the build can read each kind of document before it installs. Update by pulling the repository and running `makepkg -si` again, and remove with `sudo pacman -R papertrail`.

### Checking a download

Each release has a `SHA256SUMS` file listing every download's fingerprint, so you can check yours is the one published.

On Windows, in PowerShell in your Downloads folder, compare the result with the line for that file in `SHA256SUMS`:

```powershell
Get-FileHash .\Papertrail-*-setup.exe
```

On Linux, with `SHA256SUMS` in the same folder as the download:

```bash
sha256sum --check --ignore-missing SHA256SUMS
```

## Getting started

1. **Open Papertrail.** The first time, it welcomes you and says where your library is kept.
2. **Add a folder** of papers and books with **Add folder...**. Papertrail finds every document in it and the folders inside it, wherever they are, and never moves or changes them. Add as many folders as you like.
3. **Choose whether to look up details online.** Papertrail asks the first time. It can fill in titles, authors and journals from Crossref, OpenAlex, arXiv and Open Library, letting you check each change or not, or stay offline entirely. Change this later under **File > Settings... > Online lookups...**.
4. **Wait for the import.** The status bar shows progress, and publications appear as they're found. You can carry on using Papertrail meanwhile, and stopping is safe: it carries on next time.
5. **Find and read.** Choose a tab, filter with the sidebar, or type in the search box (the **?** beside it lists every way of searching). Double-click a publication to read it.

Moving from another computer? Choose **Import a library from another computer...** on the welcome page instead, with a `.papertrail` file exported from Papertrail there.

## Features

### Your library

- **Indexes files where they are,** in any number of folders, including on other drives. Folders are watched for new and changed files, and scanned again when Papertrail opens.
- **Formats:** PDF, EPUB, DjVu, MOBI, AZW3 (Kindle KF8), XPS, CBZ and FB2.
- **Kinds of publication,** each with its own tab: journal issues, journal articles, preprints, conference papers and proceedings, textbooks and books, theses and dissertations, technical reports, standards, case law, and others.
- **Details from your files:** titles, authors, DOIs, ISBNs and arXiv numbers are read from each document's metadata and first pages, and its kind is worked out from its first page. Anything guessed is marked for you to check.
- **Duplicates join up:** the same file in two folders is one publication with two locations, matched by its contents. Another format of the same work, such as a PDF and an EPUB, can join it too, and be separated again.
- **Covers** from each document's first page.
- **Fast with large libraries:** results, filters and searches stay well under a tenth of a second with 100,000 publications.
- **Removing:** remove publications from the library without touching their files. They stay out, through rescans, until you bring them back.
- **Unreadable files** are listed by name rather than lost, and a damaged file can't take Papertrail down with it.

### Finding things

- **A cover grid and a detailed list,** sorted by title, first author, year, date added, or how well each matches a search.
- **Filters with live counts:** tags, collections, year range, author, journal, publisher, conference, language, format and read status, and court and jurisdiction on the Case Law tab. Also **needs attention** filters: needing review, no text layer, no file, missing or offline files, unreadable files, and retracted or of concern.
- **A search box** for plain words, `"exact phrases"`, and fields: `title:`, `author:`, `abstract:`, `journal:`, `conference:`, `publisher:`, `institution:`, `tag:`, `doi:`, `isbn:`, `issn:`, `arxiv:`, `type:`, `note:`, `in:` (articles in an issue or book), and `year:` with ranges such as `year:>2010`. The **?** beside it lists them all, with examples.
- **Search inside documents:** tick **Inside documents** to find words in the text of every indexed document, and open the document at that page. Indexing happens in the background.
- **Search your highlights and notes,** with `note:` or plain words.
- **Saved searches** in the sidebar keep a search with its tab and filters, ready to show again with a click. Right-click **Saved searches** to save the current one.

### Organising

- **Tags within tags,** such as `Engineering > Structures > FEM`, with colours.
- **Collections within collections,** for courses, projects or chapters of a thesis.
- **Drag and drop** publications onto a tag or collection in the sidebar.
- **Changes to many at once:** tag, add to a collection, set read status, look up details, cite, or summarise a whole selection.
- **Read status, rating and personal notes** for each publication. Sort by rating or by when you last opened something, filter by rating, and pick up what you've opened in the last 30 days. The list can show each publication's rating, when it was last opened and how far you've read.
- **Edit any detail,** with the fields that suit each kind of publication and suggestions from the rest of your library.
- **Import references** from Zotero, Mendeley, JabRef, EndNote or anything else that exports BibTeX or RIS, with **File > Import references...**. Each entry is matched to your publications by its file, DOI, ISBN, arXiv number or title, and brings its details, tags and notes. You see what will happen first, and details you've set yourself are never changed.
- **Journal issues and books split into articles and chapters,** from their table of contents or, for an issue without one, from the DOIs on its pages. Each article opens at its own pages, is searched, tagged and cited on its own, and follows its issue into collections. Right-click an issue and choose **Show its articles** to list them in the library, in the order they're in it.
- **Related papers:** what a publication cites, and the most cited papers citing it, from OpenAlex, with those already in your library marked and the others ready to add without a file.

### Reading

- **A built-in reader** with each document in its own tab, opening where you left off.
- **Continuous, single-page or two-page** layouts, with fit to width, fit to page or any zoom.
- **Contents and page thumbnails** in a side panel, and the contents at **Ctrl+G**, searched as you type, or with a page number to go to.
- **Smart Jump:** Ctrl+click a reference such as "Figure 3", "Eq. (4)", "[12]" or "Smith et al. (2020)" for a preview of the figure, equation or reference it points at, even in a PDF without links, then go there and back. It goes to the heading or caption itself, not a mention of it in the contents, and an entry in a book's table of contents leads to its chapter or section.
- **Bookmarks** with names of your own, at **Ctrl+B**, listed in the side panel and at Ctrl+G.
- **Full screen** at F11, **pages turned** a quarter at a time at Ctrl+R, and **back and forward** at Alt+Left and Alt+Right.
- **Two documents side by side** in one tab, or the same one twice at different pages.
- **Search within a document,** stepping through every match.
- **Select and copy text,** across pages.
- **Light, dark and sepia pages,** with pictures left in their own colours on dark pages, and larger or smaller text for EPUB and other reflowable books. **File > Settings...** sets what documents open with.
- **Links** within a document, and to the web.

- **Scanned documents** have their words recognised, in the background and as you read them, so they can be searched, selected and highlighted too. English comes with Papertrail, and other languages can be fetched in **File > Settings...**.

### Highlights and notes

- **Highlight or underline** in four colours, and add notes to what you've selected, by right-clicking it.
- **Mark an area,** such as a figure or an equation, with Shift and a drag. It's kept and listed like a highlight, with a picture of it, and can be copied as a picture.
- **They survive** restarts, moving or renaming the file, and text size changes in EPUBs. They're kept in Papertrail's library, never in your files.
- **The Notes panel** lists a document's highlights and notes, and goes to each. Choose several at once to change their colour, copy, export or delete them together.
- **Every highlight and note in your library** in one searchable list, under **View > Highlights and notes...**.
- **Export to Markdown,** with each quote and note under its page.
- **Save a copy of a PDF with its highlights and notes** as ordinary PDF annotations, for any other PDF reader. The original is left untouched.

### Details from online sources

- **Optional lookups** from Crossref, OpenAlex, arXiv and Open Library, using a document's DOI, ISBN, arXiv number or title.
- **Three modes:** check each change before it's made, apply them all automatically, or stay offline.
- **A review tab** shows each suggested change side by side, and anything you've edited by hand is never overwritten.
- **Undo** any lookup, from the details panel.
- **Retraction warnings:** publications with a DOI are checked with Crossref about once a month for retractions, corrections and expressions of concern, which are shown on the publication and in a filter. A new one is pointed out, and counted in the status bar until you've looked.

### Citing

- **Copy references and in-text citations** in Harvard, APA (7th edition), IEEE or Chicago (author-date, 18th edition), with italics that paste into a word processor.
- **Reference lists** for any selection, in the style's order, with 2020a and 2020b where needed.
- **Export** BibTeX, RIS (for Zotero, Mendeley and EndNote) or CSL-JSON.
- **Case law** is cited in the Bluebook's form, or OSCOLA's if you choose it for a case, in every style.

### Summaries from Claude

- **Optional summaries** of a paper's aim, methods, results, limitations and conclusions, or of a case's facts, issues, decision and reasoning, using your own Claude API key and paid to Anthropic by you.
- **You see what will be sent, and an estimate of the cost,** before anything is sent. Only identifying details are sent, and Claude finds the paper online. Sending the paper itself needs its own confirmation.
- **Straight away, or at half price** in batches, which usually arrive within the hour. They're kept for you to review whenever you open Papertrail.
- **What you've spent,** this month and altogether, under **View > Summaries from Claude...**.

### Keeping your library safe

- **Daily backups,** keeping the last seven, plus a backup before anything that changes a lot of the library. Restore any of them from **File > Backups...**.
- **Move to another computer** with **File > Export library...** and **Import library...**: every detail, tag, collection, highlight, note and setting comes across, and files are matched by their contents, so your folders can be arranged differently there. Add to the library there or replace it.
- **One library per computer,** which only one copy of Papertrail opens at a time.

### Everywhere

- **Windows 10 and 11, and Linux** on X11 and Wayland.
- **Follows your system's light or dark mode,** or stays light or dark, under **File > Settings...**.
- **No account, no telemetry,** and nothing sent anywhere you haven't chosen.

## Where your library is kept

| Copy | Library | Covers and other cache |
|---|---|---|
| Windows | `%APPDATA%\Papertrail` | `%LOCALAPPDATA%\Papertrail\cache` |
| Windows, portable | The `Papertrail library` folder beside `papertrail.exe` | Inside it |
| Linux | `~/.local/share/Papertrail` | `~/.cache/Papertrail` |
| Flatpak | `~/.var/app/io.github.hachigodesu.Papertrail/data/Papertrail` | `~/.var/app/io.github.hachigodesu.Papertrail/cache/Papertrail` |

Backups are in a `backups` folder beside the library. Paste a Windows path into File Explorer's address bar to open it. The cache can be deleted at any time: Papertrail makes it again.

To move your library to another computer, use **File > Export library...** rather than copying these folders, since an export works whatever the other computer's folders are.

## Your files and your privacy

- **Your documents are never changed.** Highlights, notes and details are kept in Papertrail's own library.
- **Papertrail stays offline until you choose otherwise.** If you turn lookups on, it sends a document's DOI, ISBN, arXiv number or title to Crossref, OpenAlex, arXiv or Open Library, DOIs to Crossref to check for retractions, a DOI to OpenAlex when you ask for a publication's related papers, and, when you choose a language for recognising scanned text, asks GitHub for that language's file. It never sends your files, notes or tags. An email address goes to Crossref and OpenAlex only if you enter one, which they ask for to contact heavy users. A free OpenAlex key of your own, if you enter one under **Online lookups**, goes only to OpenAlex, and is kept in your computer's credential store, never in Papertrail's database, settings or exports.
- **Summaries are sent to Anthropic only when you ask,** using your own API key. You're shown what will be sent and an estimated cost first. It sends identifying details such as the DOI and title, and Claude looks for the paper online. Your file is only sent if you choose that, with a confirmation of its own. The key is kept in your system's credential store, never in the library or its exports.
- **Exports aren't encrypted.** A `.papertrail` file holds your notes and settings, so keep it as private as they are.
- **There's no telemetry,** and nothing is sent anywhere else.

## If something goes wrong

- **Windows says "Windows protected your PC":** choose **More info**, then **Run anyway**. Papertrail isn't signed with a paid certificate, so Windows warns until enough people have run it.
- **"Papertrail is already open":** only one copy can open a library at once. Close the other window, or check it isn't still closing.
- **The AppImage doesn't start:** run it from a terminal to see why. If it mentions FUSE, start it with `--appimage-extract-and-run` after its name, or use the Flatpak.
- **Online lookups can't connect on Linux:** the AppImage uses your system's OpenSSL 3, which very old distros don't have. The Flatpak includes its own.
- **A document won't open:** it's shown as unreadable, with the reason, and you can open it in another app from its right-click menu. A password-protected file, or a Kindle book with DRM, can't be opened in Papertrail.
- **Checking a copy works:** `papertrail --self-test` reads a sample of each kind of document and makes a library in a temporary folder, without opening a window or your own library, and lists what passed. On Windows, add `--report result.txt` to save what it found to a file, since Windows doesn't show it in the console.

## Building from source

Requires CMake 3.25+, Ninja, a C++20 compiler and Qt 6.12 or newer. Windows builds must stay on Qt 6.12, the last release to support Windows 10.

### Windows (MSYS2 UCRT64)

Install MSYS2 at `C:\msys64`, then in a UCRT64 shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-libmupdf mingw-w64-ucrt-x86_64-djvulibre mingw-w64-ucrt-x86_64-zlib
```

Then from the project folder:

```bash
cmake --preset windows-debug
```

```bash
cmake --build --preset windows-debug
```

```bash
ctest --preset windows-debug
```

`packaging/windows/build-package.sh` makes the installer and portable zip, with NSIS installed (`mingw-w64-ucrt-x86_64-nsis`).

### Linux

Install Qt 6.12 or newer, MuPDF, DjVuLibre, libsecret (so that an API key can be kept in the desktop's keyring), CMake and Ninja, then use the same commands with the `linux-debug` preset.

On Arch:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base libmupdf djvulibre libsecret zlib
```

Ubuntu's own Qt and MuPDF packages are too old. Install Qt with [aqtinstall](https://github.com/miurahr/aqtinstall) and pass its location to CMake with `-DCMAKE_PREFIX_PATH`, and build MuPDF with `packaging/linux/install-mupdf.sh`.

`packaging/linux/build-appimage.sh` makes the AppImage, and `packaging/flatpak/` holds the Flatpak's manifest. `papertrail --library FOLDER` opens a library kept in another folder, which is useful for trying a build without touching your own.

## Reporting problems

Report bugs on the [issue tracker](https://github.com/hachigodesu/Papertrail/issues), saying which version and system you use. See [SECURITY.md](SECURITY.md) for security problems. Please don't attach documents you don't have the right to share.

## Licence

Papertrail is free software under the GNU Affero General Public License v3.0 or later (see `LICENSE`). It includes Qt, MuPDF, DjVuLibre, libmobi, SQLite, QtKeychain and zlib, among others: see [packaging/THIRD-PARTY.md](packaging/THIRD-PARTY.md).
