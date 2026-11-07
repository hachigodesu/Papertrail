# M10 results

Tested on 7 November 2026 on Windows, Arch and Ubuntu (WSL2) on the development PC. Built while you were out, so nothing has been tried by hand yet.

You can now cite publications in four styles, and export their citations for BibTeX, Zotero and other reference managers.

## How it works

- **Copying a reference:** right-click one or more publications and choose **Cite**, or use **Cite** in the details panel. **Copy reference** puts the full reference on the clipboard, and **Copy in-text citation** puts the citation that goes in your writing, such as (Dent 2019) or [1].
- **Several at once:** a reference list comes out in the style's order, one reference per line, and the in-text citation covers them all, such as (Avery 2010; Dent 2019) or [1]–[3].
- **Italics:** what's copied carries italics for a word processor, and is also there as plain text for anything else.
- **Styles:** **Cite > Style** chooses between Harvard, APA (7th edition), IEEE and Chicago (author-date). The choice is remembered, and travels with the library's settings.
- **Exporting:** **Cite > Export citations...** for what's selected, or **File > Export citations...** for what's selected or, with nothing selected, everything shown. You choose BibTeX, RIS or CSL-JSON when saving. A large export runs in the background with a progress bar, and can be cancelled.

## Changed after hand testing

- **Articles made from an issue's contents are looked up online,** as newly imported files are, following your lookup setting. A table of contents gives titles but never authors, so until then they're cited as Anon. Their DOIs, read from their first pages, make those lookups exact.
- **Copying says when references have gaps.** If any are missing their authors, year or journal, the status bar says how many, and suggests **Look up details online**. A report or document with nobody named counts its organisation as its author.

## The styles

- **Harvard** follows the rules you gave me. Electronic sources are marked [online], with their DOI or web address and the day you copy the reference as the date accessed, as you chose. Where the place or publisher isn't known, it says s.l. or s.n.
- **APA** is the 7th edition: up to 20 authors, DOIs as links, and no place of publication.
- **IEEE** numbers references in the order you selected them, with IEEE's month abbreviations, and up to six authors before *et al.*
- **Chicago** is author-date, by the 18th edition: full names, page ranges shortened by Chicago's rules (such as 321–28), up to six authors before et al., and no place of publication.
- **In every style:** works by the same people in the same year get letters, such as 2019a and 2019b, in both the list and the in-text citations. Authors written in Chinese, Japanese or Korean characters are written in full, since they have no initials to take. A name with no given name is treated as an organisation.
- **Articles in an issue or chapters in a book** take what they don't have from it: the journal or book title, its editors, volume, issue, year, publisher and place. Their page numbers in the file are never used as the printed pages.

## The exports

- **BibTeX:** keys such as `dent2019why`, with letters added where two would clash. Special characters are escaped, and words such as DNA or iPhone are protected so a style can't change their case. Authorless entries get a `key` for sorting.
- **RIS:** for Zotero, Mendeley and EndNote, with tags as keywords.
- **CSL-JSON:** the format citation processors such as Zotero's use.
- **Tags** go into all three as keywords. Personal notes don't go into any of them.

## Decisions I made

- **The styles are written out by hand,** rather than through a CSL processor, as the plan said. There's no CSL processor for C++, and the plan leaves full CSL styles for later.
- **Titles are kept as they're stored.** APA and Harvard prefer sentence case for some titles, but changing case automatically gets names and acronyms wrong.
- **More than 500 at once are exported rather than copied,** since a reference list that long is no use pasted.
- **IEEE's in-text citation** numbers by the order of the selection, since IEEE numbers by first citation, which only you know.

## How it differs from the plan

- **"Imports cleanly into BibTeX"** is checked by running pdflatex, classic BibTeX and biber over an export of every kind of publication, plus awkward text, in the Ubuntu WSL distro. I installed TeX Live's BibTeX, biber and LaTeX there (about 130 MB), as you agreed. Every entry appears in both bibliographies. The only warnings are for the two publications that really have no author.
- **Zotero** hasn't been checked, as agreed. That's for you to try by hand.

## A problem testing found

- **The worker crashed every time it closed** after reading page text for the index, because a document it kept open outlived the engine that opened it. Nothing was lost, since it only happened on the way out, but it's fixed, with a test.

## How it's been tested

- **Automated:** all test programs pass on all three platforms. The new `test_citations` checks every kind of publication in every style against expected references written by hand in `tests/fixtures/citations/`. It also covers initials, page ranges, editions, letters after years, order, names in other scripts, escaping and the three export formats, and runs the real BibTeX and biber where they're installed. `test_library_ui` covers copying, the style menu, the details panel's Cite button and a 1,201-item export.
- **Not tested yet:**
  - **By hand,** including pasting into Word with its italics.
  - **Zotero:** importing RIS, BibTeX and CSL-JSON exports.
  - **Real publications.** The fixtures are made up, and real metadata is messier.

## Known limits

- **Only four styles.** Other variants of Harvard, or any other style, wait for full CSL support on the roadmap.
- **Titles aren't changed to sentence case** where a style asks for it.
- **Some kinds have no fixed form in a style,** such as a whole journal issue or a standard in APA, so they follow the nearest form the style has.
