Papertrail 1.5.0, a library manager and reader for academic publications, for Windows and Linux.

## What's new

- **Books downloaded as a zip of PDFs.** ScienceDirect gives a book as a zip with one PDF for each chapter and each part of its front and back matter. Papertrail now reads such a zip as one book. It works out the order of the parts from their printed page numbers, and from their names where there are none, then asks you to check it when the import finishes:
  - Each part is shown with a picture of its first page, its pages, and why it's placed where it is.
  - Drag a part, or use **Move up** and **Move down**, to move it. Untick a part to leave it out.
  - **Stitch** opens the parts as one book, with an outline entry for each part.

  The zip itself is never changed. The stitched PDF is kept in Papertrail's own folder, and **Save the stitched PDF as...** saves a copy wherever you choose. **Order of its parts...**, in the library's right-click menu and the reader's page menu, changes the order whenever you like. Highlights, notes, bookmarks and your place move with their parts. Before you leave out a part that has highlights, Papertrail warns you, and putting the part back brings them back. Other zips in your folders are left alone.
- **A collection's menu.** Right-click a collection in the sidebar for two new options:
  - **Export citations...** writes BibTeX, RIS or CSL-JSON for everything in the collection, and in the collections inside it, to a file named after it.
  - **Show its highlights and notes** opens the highlights window with that collection chosen.

## What's changed

- **Text set at an angle is drawn properly.** Words turned about 45 degrees, such as a chart's diagonal axis labels, were drawn as large black shapes over the page with some versions of FreeType, including the one in the Windows download. They're now drawn as words, in the reader, thumbnails and covers.
- **A library opened in 1.5.0 can't be opened in 1.4.0 again,** because 1.5.0 remembers the order of a book's parts. Papertrail makes a backup before upgrading your library.

## Downloads

| File | For |
|---|---|
| `Papertrail-1.5.0-setup.exe` | Windows 10 and 11. Installs just for you, or for everyone |
| `Papertrail-1.5.0-portable.zip` | Windows 10 and 11, from any folder or a USB stick, keeping its library beside it |
| `Papertrail-1.5.0-x86_64.AppImage` | Most Linux distros, from Ubuntu 22.04 and Debian 12 on |
| `Papertrail-1.5.0.flatpak` | Any Linux distro with Flatpak. Installs the KDE runtime from Flathub |
| `SHA256SUMS` | Checking a download is the one published |

The Windows downloads aren't code-signed, so Windows may say "Windows protected your PC" the first time. Choose **More info**, then **Run anyway**.

See the [README](https://github.com/hachigodesu/Papertrail#readme) for how to install it and what it sends online.
