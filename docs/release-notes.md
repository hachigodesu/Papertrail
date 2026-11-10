Papertrail 1.2.0, a library manager and reader for academic publications, for Windows and Linux.

## What's new

- **Related papers.** Choose **Related** in a publication's details, or **Related papers...** from its right-click menu, to see what it cites and the most cited papers that cite it, from OpenAlex. Those already in your library are marked. Others can be added without a file, ready to cite, and they join their file when it turns up in your folders. Only the publication's DOI is sent, only when you ask, and only with online lookups on.
- **Smart Jump from a table of contents.** Ctrl+click an entry in a book's contents to go to that chapter or section, even where the file has no links.
- **Smart Jump goes to the thing itself.** A mention of "Appendix E" or "Section 2.1" now goes to its heading, however far away, rather than to its line in the contents. Contents pages, lists of figures and running heads are passed over.
- **Bookmarks.** Ctrl+B keeps the place you're reading, named after its page. They're listed in the side panel's Bookmarks tab, where you can rename and delete them, and in Ctrl+G. A book whose pages move with its text size keeps them in the right place.
- **Full screen.** F11 shows only the document. Escape or F11 comes back.
- **Turning pages.** Ctrl+R and Ctrl+Shift+R turn a document's pages a quarter turn, for a scan the wrong way up or a table on its side. Each document remembers how it's turned.
- **Forward as well as back.** Alt+Right, the → button, or a mouse's forward button goes back to where Back came from.
- **Reading at a glance.** Sort by **Rating** or **Last opened**. Filter by rating in the sidebar, and choose **Opened in the last 30 days**, beside the read statuses, to pick up where you were. Right-click the list's header to show **Rating**, **Last opened** and **Progress** columns.
- **One issue's articles exactly.** **Show its articles** now shows only that issue's articles, not those of issues with a longer title containing its own, and a saved search keeps them.

## What's changed

- Searching with `in:` still finds articles by what they're in.

Opening your library in 1.2.0 adds room for bookmarks and related papers to it, after backing it up. Once it has, 1.1.0 can't open it.

## Downloads

| File | For |
|---|---|
| `Papertrail-1.2.0-setup.exe` | Windows 10 and 11. Installs just for you, or for everyone |
| `Papertrail-1.2.0-portable.zip` | Windows 10 and 11, from any folder or a USB stick, keeping its library beside it |
| `Papertrail-1.2.0-x86_64.AppImage` | Most Linux distros, from Ubuntu 22.04 and Debian 12 on |
| `Papertrail-1.2.0.flatpak` | Any Linux distro with Flatpak. Installs the KDE runtime from Flathub |
| `SHA256SUMS` | Checking a download is the one published |

The Windows downloads aren't code-signed, so Windows may say "Windows protected your PC" the first time. Choose **More info**, then **Run anyway**.

See the [README](https://github.com/hachigodesu/Papertrail#readme) for how to install it and what it sends online.
