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
