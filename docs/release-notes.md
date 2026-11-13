Papertrail 1.4.0, a library manager and reader for academic publications, for Windows and Linux.

## What's new

- **Read aloud.** **View > Read aloud** (Ctrl+Shift+Space) reads from the selection, the reading ruler or the top of the page, or from wherever you click, marking each sentence as it's said and going on through the pages. A player along the bottom goes back or on a sentence, pauses and stops, and changes the speed and volume as it reads. Running heads, page numbers and a figure's labels are passed over, a table is read a row at a time, and "Fig." is said as "Figure" and "pp. 3–5" as "pages 3 to 5", with reference numbers such as "[12]" left out. Scanned pages are read once their text is recognised.
- **Natural voices.** Kokoro's voices sound far more like a person than your computer's own. They're 367 MB, so they don't come with Papertrail: it offers to fetch them from Hugging Face the first time you read aloud, or from **File > Settings...**, only with online lookups on. Once they're here they read on your computer, and nothing you read is sent anywhere. There are 28 English voices, American and British. Without them, Papertrail uses Windows' voices, or on Linux a voice that comes with it.
- **A reading ruler.** **View > Reading ruler** (Ctrl+L) dims everything but the line or paragraph you're reading. Up and Down move it, a click chooses a line, and it goes on to the next page. It passes over running heads, page numbers and notices down the side of the page, and takes a whole figure or a whole table at once.
- **Every highlight in a window of its own.** **View > Highlights and notes...** (Ctrl+Shift+N) stays open beside the library while you read. Search it, or choose by colour, kind, collection, tag or publication, change a highlight's colour or note there, and export what's shown as Markdown. **Show its highlights** in the library's right-click menu shows one publication's.
- **Checking the library.** **File > Check the library...** lists files that are missing or can't be read, publications waiting for their files, files that aren't in any publication, and publications sharing a DOI, ISBN, arXiv number or PubMed id. Each has its fixes, such as finding a file, reading one again, keeping one out, combining two, or saying they aren't the same, which Papertrail then remembers.
- **Court cases looked up in sources of judgments.** With lookups online, a case's details come from a source for its jurisdiction:
  - **United Kingdom:** Find Case Law, from The National Archives, for cases you choose to look up, since its licence doesn't cover searching it for a whole library.
  - **European Union:** EUR-Lex, by the judgment's ECLI or case number.
  - **United States:** CourtListener, with a free token of your own from courtlistener.com.
  - **Canada:** CanLII, with a key of your own, which CanLII gives on request.

  Keys go in **File > Settings... > Online lookups...**, and Papertrail says when a case you look up needs one. Judgments of the EU's courts and Canada's, including those printed in the Supreme Court Reports, are now recognised as cases on import.
- **Partly scanned documents.** The scanned pages of documents that otherwise have text of their own are now recognised in the background too, after wholly scanned documents. Documents indexed by 1.3.0 are found without being indexed again.
- **An OpenAlex key of your own.** A free key from openalex.org, entered in **File > Settings... > Online lookups...**, gives your lookups and related papers OpenAlex's allowance for you alone, rather than the smaller one shared by everyone on the same internet address. It's kept in your computer's credential store and sent only to OpenAlex.

## What's changed

- **A case is never looked up in Crossref or OpenAlex.** Before, a case could be searched for by its name there, and a paper with a similar title could replace the case's details.
- **Opening your library in 1.4.0 means 1.3.0 can't open it again,** since 1.4.0 remembers publications you've said aren't the same. Papertrail makes a backup before upgrading it.
- **The downloads are about 25 MB larger,** for what makes the natural voices. The voices themselves are only fetched if you ask.

## Downloads

| File | For |
|---|---|
| `Papertrail-1.4.0-setup.exe` | Windows 10 and 11. Installs just for you, or for everyone |
| `Papertrail-1.4.0-portable.zip` | Windows 10 and 11, from any folder or a USB stick, keeping its library beside it |
| `Papertrail-1.4.0-x86_64.AppImage` | Most Linux distros, from Ubuntu 22.04 and Debian 12 on |
| `Papertrail-1.4.0.flatpak` | Any Linux distro with Flatpak. Installs the KDE runtime from Flathub |
| `SHA256SUMS` | Checking a download is the one published |

The Windows downloads aren't code-signed, so Windows may say "Windows protected your PC" the first time. Choose **More info**, then **Run anyway**.

See the [README](https://github.com/hachigodesu/Papertrail#readme) for how to install it and what it sends online.
