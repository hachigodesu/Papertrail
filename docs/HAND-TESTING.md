# Testing M10 and M11 by hand

M10 (citations) and M11 (moving a library between computers) were built and tested automatically on Windows, Arch and Ubuntu, with nobody using them. This list is what only a person can check, most important first. Details are in `docs/M10-results.md` and `docs/M11-results.md`.

Nothing is committed yet.

## Before you start

Close Papertrail if it's open, then build it with the Bash shell:

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
```

```bash
cmake --build --preset windows-debug -j 8
```

```bash
tools/windows/run-debug.cmd
```

**Opening your library with this build adds two tables,** for publications waiting for their files and for suggested files, so an older build can't open it afterwards. Copy your library folder first if you want a way back.

## 1. Citations (M10)

1. Right-click a journal article and choose **Cite > Copy reference**. Paste it into Word and into a plain text editor. Check the italics come through in Word, and that the reference follows your Harvard rules, including [online], the DOI and today's date as accessed.
2. Try **Cite > Style** with APA, IEEE and Chicago, and copy again. Check against what you know of each.
3. Try **Copy in-text citation** with one publication, then several selected together.
4. Try a book, a chapter or article inside an issue, a thesis and a report, which have their own forms.
5. Try two papers by the same author in the same year. They should become 2020a and 2020b.
6. Use **Cite** in the details panel too.
7. Make articles with **From contents...** in an issue. With online lookups on, they should be looked up and gain their authors. Copying references for ones still missing details should say so in the status bar.
8. **Zotero:** export some publications with **File > Export citations...** as RIS, then as BibTeX, then as CSL-JSON, and import each into Zotero. Check the item types, authors, journal, pages, DOI and tags.
9. Export everything with nothing selected, and notice how long a large library takes.

## 2. Moving a library (M11)

The real test is two computers. With one, you can still try most of it.

1. **Export:** **File > Export library...**. Read what it says, then save the file. Notice how long it takes and how large the file is.
2. **Import into a new library:** start Papertrail with a different, empty library, if you have a way to (or on another computer). Import the file. Then add a folder with some of the same documents, ideally renamed and moved around. Check that:
   - the report's counts make sense
   - publications are connected to their files once the scan finishes
   - tags, collections, highlights, notes, summaries, ratings and places came with them
   - anything whose file isn't found stays out of sight, **Library folders** says how many are waiting, and each appears once its file is added
3. **Import into your own library:** import the same file back into the library it came from. It should report everything as already here, and change nothing.
4. **Edit on both sides:** change a publication's details, export, change them again, then import the older export. Your newer change should stay.
5. **Replace:** if you're willing, try **Replace this library with it** on a copy of your library. Check the backup appears beside it, named `library-before-import-` and the date.
6. **Several folders:** in **Library folders**, choose two or more folders and use **Remove**. There should be one question naming them all.
7. **Suggested files:** if you have another download of a paper with a different file, check it's suggested under **File > Check suggested files...**, and that accepting it joins them.

## 3. Your second round

1. **Grid selection:** in the grid, drag from inside the first cover to past the bottom right of a short last row. Every cover from there on should be chosen, including the short row.
2. **Issue titles:** open your library. Issues whose titles were a cover's words, such as "Publication Information" or a line of numbers, should now read like "Journal, Vol. 18, No. 5 (October 2026)" once their journal is known. Where it isn't yet, looking up one of their articles fills it in. A title you typed yourself is never changed.
3. **Suggestions while typing:** in **Edit details**, type two or more letters of a journal, publisher, place, institution or similar. Names already in the library should be offered, most used first. In **Authors** and the other people fields, each line is suggested for separately.
4. **Ways to search:** click **?** beside the search box. Choosing an entry adds it to the search, with its value chosen ready to type over.

## Things I decided that you may want to change

- **Copying is limited to 500 at once,** with exporting for more.
- **IEEE numbers by the order you selected them.**
- **A publication found by its contents takes the export's details,** even though the one made by scanning here is newer.
- **Suggested files have their own list** under **File**, rather than joining the review of online lookups.
- **Open documents are closed during an import,** and the library can't be used until it finishes.
- **Suggestions start at two letters,** and match anywhere in a name, not only its start. People are matched by the start of their family or given name.
- **An issue's title is made in one form for every style,** "Journal, Vol. X, No. Y (Month Year)", leaving out whatever isn't known.

## When it looks right

Tell me, and I'll commit M10 and M11 as one signed commit each, and push.
