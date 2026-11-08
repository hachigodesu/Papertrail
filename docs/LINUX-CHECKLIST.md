# Trying Papertrail on a Linux desktop

The automated tests run Papertrail without a screen, and the containers only check it starts. This list is what needs a real desktop. Try both packages if you can, and in both a Wayland and an X11 session if your desktop offers both (most offer a choice on the login screen).

Write down your distro, its version, the desktop (GNOME, KDE Plasma and so on), and whether the session is Wayland or X11 (`echo $XDG_SESSION_TYPE`). Note anything that looks wrong, with a screenshot, rather than trying to fix it.

## Getting the packages

Download `Papertrail-<version>-x86_64.AppImage` and `Papertrail-<version>.flatpak` from the draft release on GitHub. Before there is one, they're attached to the latest **AppImage** and **Flatpak** runs under the repository's **Actions** tab.

## 1. The AppImage

1. Make it executable and check it can read documents:

   ```bash
   chmod +x Papertrail-*-x86_64.AppImage
   ```

   ```bash
   ./Papertrail-*-x86_64.AppImage --self-test
   ```

   Every line should start with `PASS`.

2. Open it by double-clicking it in your file manager. If nothing happens, run it from a terminal and note what it prints. On some distros AppImages need `libfuse2` installed.
3. The window should have Papertrail's icon in the title bar and taskbar or dock, the welcome page should say where the library is kept, and the text should be sharp at your display's scaling.

## 2. Using it

Use a copy of a few folders of papers and books, not your only copy, though Papertrail never changes them.

1. **Add folder...** and choose one. Choose whether to look things up online.
2. Check the covers appear, and the grid and list both scroll smoothly.
3. Open a PDF, an EPUB and anything else you have (DjVu, MOBI, AZW3). Check pages look right, text can be selected and copied, and search finds words.
4. Highlight something and add a note. Close Papertrail, open it again, and check the highlight is there.
5. Right-click a PDF with highlights, choose **Save a copy with highlights...**, and open the copy in another PDF viewer.
6. Switch your desktop between light and dark, if it can, and check Papertrail follows.
7. Choose **File > Export library...**, then check the file chooser looks like your desktop's own.
8. If you use summaries: enter your Claude API key, close and reopen Papertrail, and check it's still known. It's kept in your desktop's keyring (GNOME Keyring or KWallet).
9. **Help > About Papertrail** should show the version and link to the source. **Show licences** should open a folder of licence texts.

## 3. The Flatpak

1. Install it, which also installs the KDE runtime from Flathub if you don't have it:

   ```bash
   flatpak install --user Papertrail-*.flatpak
   ```

   ```bash
   flatpak run io.github.hachigodesu.Papertrail --self-test
   ```

2. Open it from your desktop's app menu. It should be listed as Papertrail with its icon.
3. Repeat section 2. The Flatpak keeps its own library, separate from the AppImage's, in `~/.var/app/io.github.hachigodesu.Papertrail`.
4. Check it can add a folder anywhere in your home folder, and on another drive if you have one.
5. Exporting and saving a copy with highlights should go through your desktop's file chooser and work anywhere you choose.

## 4. Removing them

```bash
flatpak uninstall --user io.github.hachigodesu.Papertrail
```

The AppImage is just the one file. Removing either leaves its library, which you can delete by hand.

## When you're done

Tell me what you found, with the details from the top of this page, and I'll fix what's wrong before the release.
