# M12 results

Built and tested on 9 November 2026 on the development PC. The Windows packages were built and self-tested here. The Linux packages are built by CI. Nothing has been installed by hand yet: see `docs/HAND-TESTING.md` for Windows and `docs/LINUX-CHECKLIST.md` for a Linux desktop.

M12 turns Papertrail into something to install: a Windows installer and portable zip, an AppImage, a Flatpak and an AUR package, with a first-run welcome, licences, a self-test every package runs, tests against damaged files, and a release workflow that makes a draft release.

## Decisions you made

- **The installer asks** whether to install just for you (no administrator rights, the default) or for everyone.
- **Unsigned for now,** so Windows shows its SmartScreen warning the first time. The README explains what to do.
- **GitHub Releases only.** The Flatpak manifest and PKGBUILD are in the repository, but nothing has been submitted to Flathub or the AUR.
- **Linux by hand on your own machine,** following `docs/LINUX-CHECKLIST.md`.
- **The name stays Papertrail.** SolarWinds sells a log-management service called Papertrail, papertrail.io is a safety-inspection app, and a small citation tool on SourceForge is called PaperTrail. You judged the first two to be in different categories. The SourceForge one is in the same category but has no reviews and isn't actively developed.

## The program

- **Version 1.0.0.**
- **First run:** an empty library now shows a welcome, with **Add folder...**, **Import a library from another computer...**, and where the library is kept.
- **`--self-test`** checks a copy without opening a window or the user's library: the worker reads a PDF, an EPUB and a DjVu built into the program and writes their covers, the covers decode, a library in a temporary folder is made and searched, and secure connections are available. It exits with 0 if everything passed, and `--report FILE` writes what it found to a file. Every package runs it.
- **`--library FOLDER`** opens a library kept in that folder.
- **Portable mode:** a `portable.txt` beside the program keeps the library in a `Papertrail library` folder beside it. The portable zip includes one.
- **About** gives the version, the licence, a link to the source, the main libraries included, and **Show licences**, which opens the installed licence texts.
- **An icon** in the window, taskbar and Windows' file details, drawn from the placeholder SVG by `packaging/make_icons.py` with plain Python. It's still the placeholder design.

## Packages

| Package | Built by | Checked by |
|---|---|---|
| Windows installer (`Papertrail-1.0.0-setup.exe`, 54 MB) | `packaging/windows/build-package.sh`, NSIS | The self-test with only Windows' folders on PATH. CI also installs it silently, self-tests the installed copy and uninstalls it, checking nothing is left |
| Portable zip (`Papertrail-1.0.0-portable.zip`, 75 MB) | The same script | The same files and self-test |
| AppImage | `packaging/linux/build-appimage.sh` on Ubuntu 22.04 | Its self-test and start-up on Ubuntu 22.04 and 24.04, Debian 12, Fedora, openSUSE and Arch |
| Flatpak | `packaging/flatpak/io.github.hachigodesu.Papertrail.yml`, KDE runtime 6.11 | AppStream and desktop file validation, then installed and self-tested in its sandbox, in CI |
| AUR | `packaging/arch/PKGBUILD`, with Arch's own MuPDF and DjVuLibre | `makepkg` runs the self-test in `check()`. `test-pkgbuild.sh` builds it from the checkout |

- **The installer** offers just this user (into `%LOCALAPPDATA%\Programs\Papertrail`) or everyone (into Program Files), a desktop shortcut if wanted, and **Open Papertrail** at the end. It's listed in **Settings > Apps** with its version and icon. **Uninstalling deletes only the files it installed, by name,** so an install folder chosen by mistake, such as `C:\`, loses nothing else. It keeps the library, notes, highlights and backups, and says so.
- **Licences:** each package includes Papertrail's licence, `packaging/THIRD-PARTY.md`, and the licence text of every library it bundles. On Windows they're taken from each DLL's MSYS2 package, so the list follows whatever's bundled. MSYS2 has no text for DjVuLibre, so its `COPYING` is kept in `packaging/windows/licenses/`, taken from its release with the hash checked. The script fails if any library's licence can't be found.
- **Secure connections on Windows use Windows' own Schannel,** through Qt's plugin, so OpenSSL isn't shipped there.
- **The Flatpak can read every file but change none** (`--filesystem=host:ro`). Exports and highlighted copies are saved through the desktop's file chooser.
- **One app id,** `io.github.hachigodesu.Papertrail`, names the desktop file, icon and AppStream file in every Linux package, so the window matches its icon on Wayland.

## Releasing

Pushing a tag such as `v1.0.0` runs `.github/workflows/release.yml`. It checks the tag matches `CMakeLists.txt`, the PKGBUILD and the AppStream file, builds every package through the same workflows that run on each push, and makes a **draft** release with the files, a `SHA256SUMS`, and `docs/release-notes.md`. A draft is only visible to you until you publish it.

## Hardening

- **Damaged files:** `test_damaged_files` takes every sample document, cuts it short, zeroes part of it, splices it into itself, adds rubbish to its end, and changes random bytes, then runs each copy through the import's worker and the reader's process. Every request has to finish within its limit, and a good file has to work afterwards. In the normal suite that's 370 damaged files through the worker and 111 through the reader, in about 20 seconds. A larger run of 1,406 found one file that hung a parser, which the worker's time limit stopped as it should, and nothing that crashed or hung Papertrail itself.
- **Sanitizers:** `sanitizers.yml` builds Papertrail on Arch with AddressSanitizer and UndefinedBehaviorSanitizer and runs every test, so memory errors and undefined behaviour in Papertrail's own code fail CI.
- **Speed,** with a release build on a 100,000-publication library, all within target:
  - results by any sort: 5 ms, and 7 ms on a tab (target 100 ms)
  - filters: 7 to 63 ms, the slowest by format
  - searches: 1 to 42 ms, and 147 to 154 ms ranked by relevance (target 250 ms)
  - searching inside 83,000 pages: 43 to 264 ms, the slowest an exact phrase
  - with a tenth of the library hidden, or a collection chosen: under 40 ms

## How it differs from the plan

- **The Flatpak and AUR package aren't published,** as you chose. Their files are ready for when you want to.
- **Windows packages use MSYS2's build of Qt and its libraries,** deployed with `windeployqt6`, rather than a separate build.
- **Crash reporting** wasn't added. Every document is already parsed in a worker that's restarted after a crash, and the damaged-file test and sanitizers check that holds. Nothing is ever sent anywhere, so a crash report would have to be one the user sends themselves.

## From hand testing

- **Several highlights and notes can be chosen at once** in the reader's **Notes** list, with Ctrl and Shift clicks or Ctrl+A. Right-clicking them offers a colour for all of them, **Copy**, **Export to Markdown...** and **Delete**, and Ctrl+C and Delete work too. Deleting several asks first. Each change is made in one go, the choice is kept afterwards, and clicking one of several doesn't jump to it. Copying gives each one's words, page and note as plain text.
- **Institution is no longer a filter** in the sidebar, since it only means something for theses and filled with whatever details guessed. It's still shown in a publication's details, and `institution:` in the search box still finds it. The sidebar's background counts do one query fewer.

## Problems found

- **The AppImage couldn't find its worker,** so it couldn't import or show any document. It starts the program from the image's root while the worker is in `usr/bin`, and the old smoke test only checked the window stayed open. The new self-test caught it in CI. The worker is now also looked for where an AppImage keeps it, using the `APPDIR` its runtime sets.
- **The installer ignored a folder given with `/D`** when installing just for the user, since choosing the mode set its default folder. It now keeps one that's given, which CI's silent install needs.
- **The sanitizers found two places where Papertrail used a widget while it was being destroyed,** both when closing a window: the reader's contents list asked its view for its viewport from an event filter, and the review tab's closing reached into the main window after it had gone. Event filters now compare against what they saved, and the review tab is held by a `QPointer`. The sidebar's and tab bar's event filters had the same pattern and were changed too.
- **The AppImage's self-test failed on Debian 12's container,** which has no OpenSSL, unlike any desktop. The AppImage uses the system's, as before, so the container now installs it. The other five distros already had it.
- **GitHub's runner had no `eu-strip`,** which `flatpak-builder` strips programs with, so the Flatpak workflow installs `elfutils`.
- **A test expected exactly one failed import** with a missing worker, but on Linux the folder watcher can scan again and fail again. It now expects at least one.
- **Flathub has no KDE 6.12 runtime yet,** only KDE's source for one, so the Flatpak's first CI build failed. It now uses the 6.11 runtime, with Qt 6.11.3. Papertrail only asked for 6.12 by choice, to match Windows, so a setting lets the Flatpak build with 6.11. All the tests pass with Qt 6.11.3 on Ubuntu once one test used a drag event in a way 6.11 also has. It should move to 6.12 once Flathub publishes it.
- **WSL couldn't start its VM** for a while, saying Windows had too few resources, though 6 GB was free. It recovered by itself. If it happens again, closing other programs or limiting WSL's memory in `.wslconfig` would help.
- **MSYS2 has no licence text** for DjVuLibre, and keeps ICU's and OpenJPEG's under other names, so the script now finds licence files from each package's own file list.

## How it's been tested

- **Automated:** all 19 test programs pass on Windows, Arch and Ubuntu. The speed tests pass with a release build on Windows.
- **The Windows packages** were built here, and the packaged program's self-test passed with nothing but Windows' own folders on its path.
- **The AUR package** was built on Arch from this checkout with `test-pkgbuild.sh`. Its self-test passed in `check()`, and it installs the program, its worker, the desktop file, icon, AppStream file and licences where Arch expects them.
- **Not tested yet:**
  - Installing anything by hand, on Windows or Linux.
  - The new CI workflows (Windows package, Flatpak, sanitizers and release), which run once this is pushed.
  - The Flatpak, which has only been written. CI builds it for the first time.
  - A clean Windows machine without MSYS2. The self-test runs without MSYS2 on PATH, but MSYS2 is still installed here.

## Known limits

- **SmartScreen warns** about the unsigned installer until enough people have run it.
- **The AppStream release date** is set to today, and should be updated to the real date when the release is published.
- **The icon is a placeholder.**
