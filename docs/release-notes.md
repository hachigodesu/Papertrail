Papertrail 1.3.0, a library manager and reader for academic publications, for Windows and Linux.

## What's new

- **Scanned documents can be searched.** Papertrail now recognises the words in documents that are only pictures of pages, such as scanned books and old papers. Once it has, you can search inside them, select and copy their text, highlight it, and use Smart Jump in them, as in any other document.
- **In the background, and as you read.** After indexing your documents' text, Papertrail works through scanned ones slowly, at low priority, a page at a time, and carries on where it left off next time. The status bar shows how far it's got, and Stop stops it. A scanned page you're reading is recognised straight away.
- **Old papers with poor text.** Some scanned papers come with a text layer of their own that's full of mistakes, so searching them misses words that are plainly on the page. **Recognise its text** in the library's right-click menu, or **Recognise this document's text** in the reader's, reads every page again from the pictures and uses those words in place of the file's own, starting with the page you're reading.
- **Languages.** English comes with Papertrail. **File > Settings...** lists 26 more, such as French, German, Chinese and Russian, fetched from Tesseract's own collection when you choose them. That's the only thing fetched, only after you agree, and only with online lookups on. Each file is checked against the one expected before it's kept.
- **Your choice.** **File > Settings... > Scanned documents** turns it off on this computer. Nothing leaves your computer, and your files aren't changed: the recognised words are kept in Papertrail's own cache.

## What's changed

- The **No text** filter still lists scanned documents, since what Papertrail recognises isn't the files' own text.

If you're coming from 1.1.0, see 1.2.0's notes too: it added related papers, bookmarks, full screen, turned pages and more. Opening your library in 1.2.0 or later means 1.1.0 can't open it again.

## Downloads

| File | For |
|---|---|
| `Papertrail-1.3.0-setup.exe` | Windows 10 and 11. Installs just for you, or for everyone |
| `Papertrail-1.3.0-portable.zip` | Windows 10 and 11, from any folder or a USB stick, keeping its library beside it |
| `Papertrail-1.3.0-x86_64.AppImage` | Most Linux distros, from Ubuntu 22.04 and Debian 12 on |
| `Papertrail-1.3.0.flatpak` | Any Linux distro with Flatpak. Installs the KDE runtime from Flathub |
| `SHA256SUMS` | Checking a download is the one published |

The Windows downloads aren't code-signed, so Windows may say "Windows protected your PC" the first time. Choose **More info**, then **Run anyway**.

See the [README](https://github.com/hachigodesu/Papertrail#readme) for how to install it and what it sends online.
