Papertrail 1.1.0, a library manager and reader for academic publications, for Windows and Linux.

## What's new

- **Smart Jump.** Ctrl+click a reference such as "Figure 3", "Table 2", "Eq. (4)", "Section 2.1", "[12]" or "Smith et al. (2020)" to see what it points at, even in a PDF without links: the figure with its caption, the table, the equation, or the whole entry in the references. Click the preview or press Enter to go there, and Alt+Left to come back.
- **Contents at Ctrl+G.** **View > Go to...** shows a document's contents over its pages. Type to find an entry, ignoring case and accents, or type a page number, then press Enter.
- **Two documents side by side.** Right-click a publication and choose **Open beside**, or right-click a document's tab and choose **Show beside**. The same document can be on both sides, each at its own page. **Separate** makes two tabs again.
- **Areas as highlights.** Shift and a drag marks a rectangle over a figure, table or equation, as does **Mark an area** in the page's right-click menu. Areas are kept and listed like highlights, with a picture of each in the Notes panel. **Copy picture** puts one on the clipboard, and a marked copy of a PDF includes them.
- **Case law.** Judgments have their own type and tab, with their court, jurisdiction, neutral citation and case number. Every style cites a case the Bluebook's way, or OSCOLA's if you choose it for that case. Judgments are recognised when they're imported, and marked for you to check. A summary from Claude of a case is a brief of the judgment.
- **Import references from other managers.** **File > Import references** reads a BibTeX or RIS export from Zotero, Mendeley, JabRef or EndNote. It matches each entry to your publications by file, DOI, ISBN, arXiv number or title, and brings in its details, tags and notes. It shows what will happen first, and backs up the library. Details you've set yourself are never changed, and any change can be undone from a publication's details.
- **New retractions are pointed out.** When a check finds a new retraction, withdrawal or expression of concern, a message names the publication. The status bar keeps count until you look, even after a restart.

## What's changed

- A lookup choice made for one folder, such as keeping it offline, now lasts until that folder's own import has finished, even if another import was already running.
- New retractions aren't announced in the status bar for 30 seconds any more, since the new message and count replace that.

Opening your library in 1.1.0 adds case law to it, after backing it up. Once it has, 1.0.2 can't open it.

## Downloads

| File | For |
|---|---|
| `Papertrail-1.1.0-setup.exe` | Windows 10 and 11. Installs just for you, or for everyone |
| `Papertrail-1.1.0-portable.zip` | Windows 10 and 11, from any folder or a USB stick, keeping its library beside it |
| `Papertrail-1.1.0-x86_64.AppImage` | Most Linux distros, from Ubuntu 22.04 and Debian 12 on |
| `Papertrail-1.1.0.flatpak` | Any Linux distro with Flatpak. Installs the KDE runtime from Flathub |
| `SHA256SUMS` | Checking a download is the one published |

The Windows downloads aren't code-signed, so Windows may say "Windows protected your PC" the first time. Choose **More info**, then **Run anyway**.

See the [README](https://github.com/hachigodesu/Papertrail#readme) for how to install it and what it sends online.
