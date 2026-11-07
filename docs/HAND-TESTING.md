# Testing M7 to M9.5 by hand

M7, M8, M9 and M9.5 were built overnight and tested automatically on Windows, Arch and Ubuntu, with nobody using them. This list is what only a person can check, most important first. Details of each are in `docs/M7-results.md`, `M8-results.md`, `M9-results.md` and `M9.5-results.md`.

## Before you start

The work is on the local branch `overnight/m7-to-m9.5`, with one unsigned commit per milestone. Nothing has been pushed, and `main` is unchanged.

Close Papertrail if it's open, then build it. The first configure downloads QtKeychain, and these three commands use the Bash shell:

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
```

```bash
cmake --preset windows-debug
```

```bash
cmake --build --preset windows-debug -j 8
```

```bash
tools/windows/run-debug.cmd
```

**Opening your library with this build changes it, and an older build can't open it afterwards.** It adds tables for collections and summaries, and starts reading your documents' text into `fulltext.db` beside your library. If you want a way back, copy your library folder first.

## 1. Summaries with the real Claude API (M9.5)

This has never run against the real API, so it's the most likely place to find a problem. **It costs real money, usually a few cents each on Sonnet at High.**

1. Enter your key under **File > Claude API key...**. It should say the key has been saved.
2. Close and reopen Papertrail, then check **Claude API key...** still says a key is saved.
3. Select an **open-access paper with a DOI**, and choose **Summarize with Claude...**. Check that the details listed are all that should be sent.
4. Note the estimate, and generate.
   - Does it finish, or show an error? An error here most likely means the request's format needs fixing, so send me the message.
   - Are all five sections filled in, and accurate compared with the paper?
   - Does it say **full text**, and are the sources right?
   - **Compare the actual cost with the estimate,** and with your Anthropic usage page. The estimates are my judgement, and the most likely thing to need changing.
5. Try a **paywalled paper**. It should say it could only read the abstract, and say so in the sections it can't cover. **Check it hasn't made anything up.**
6. Try something **hard to find**, such as an internal report with no DOI. It should say it couldn't find it, and not invent a summary.
7. From that result, choose **Send the paper to Claude...**. Check that the warning, the amount spent and the new estimate make sense, then send it and compare.
8. **Accept** one and **Discard** another. The accepted one should be in the details panel, marked as generated, and still there after a restart.
9. Try a **biomedical paper**. If Claude's safeguards decline, it should say so and offer another go with a new estimate.
10. Try **Haiku** and **Opus** once each, to see that other models work.

## 2. Articles in a real journal issue (M7)

The only sample is a tiny generated PDF, so real tables of contents are the main unknown.

1. Set a real issue's type to **Journal issue** in its editor, then choose **From contents...** in the details panel.
2. Is the level it chooses the one that lists articles? Do page ranges look right? Are covers and indexes left unticked?
   - **If the contents only number the articles,** the titles should fill in from each article's first page, marked **Title from its first page**. Check they're right, and whether any picked up a running header or a journal name instead.
   - **If you already made articles with number titles,** they should be offered as **Names the article already made**, and confirming should rename them rather than add more.
   - Check whether articles got their **DOI**.
   - The issue's **volume and issue numbers** should be filled in, and shown on its card, such as "Vol. 7, No. 11, Nov 2026".
   - Articles should get **covers of their own first pages** after a moment.
   - Opening the issue, its **Contents** panel should show the articles' titles rather than numbers.
   - Opening an article from **Journal Articles** should show **only its pages**, numbered from 1, with **Whole issue** in the toolbar to see the rest. Opening it from the issue's **Articles** list should show the whole issue.
   - Document tabs should **drag into a new order**, with **Library** always staying first.
   - Long titles in the reader's **Contents** panel should wrap, and rewrap when the panel is widened.
3. Create the articles. Double-click one in the details panel: does it open at the right page?
4. Check they appear under **Journal Articles**, and can be tagged and added to a collection.
5. Try **Mark as checked** on several, since they all start as needing checking.
6. Try a book with chapters, and a volume with **hundreds** of entries if you have one.

## 3. Search inside documents on your real library (M8)

Typing in the search box while reading should go back to the library and show the results.

1. Watch the status bar while indexing runs. Note how long it takes and whether the PC stays usable. **Stop indexing** should work, and it should carry on after a restart.
2. Check how large `fulltext.db` gets beside your library.
3. Tick **Inside documents**, and search for a phrase you know is in a paper. Does **Found inside** show it with sensible text? Does double-clicking open the right page?
4. Try a two-column paper, a phrase across a line break, and a common word, and notice how long the common word takes.
5. Check the **No text** filter is right for a scanned file.

## 4. Highlights and notes (M9)

1. Drag to select text in a PDF. Use **Highlight**, and right-click for colours, **Underline** and **Add a note...**. Right-clicking a highlight should tick its current colour.
2. Check how highlights look in **dark** and **sepia** modes, as I've only seen them in light.
3. Close and reopen the document, then restart the app. They should still be there.
4. Move or rename the file and rescan. They should still be there.
5. In an **EPUB**, highlight something, make the text bigger and smaller, and check the highlight follows the words.
6. Try a selection across two pages.
7. Use **Export to Markdown...** on the **Notes** tab, and read the result.
8. Highlight something while reading an article, then open its volume. It should show there too.

## Things I decided that you may want to change

- **Indexing is on by default,** and starts on your library the first time this build runs. **File > Index document text** turns it off.
- **Summaries, highlights and search all live in new sections** of the details panel, which is getting long. It may want tidying up after you've seen it.
- **A note has to be attached to selected text.** There are no free-floating notes on a page.
- **Volumes show their articles in a list in the details panel,** rather than expanding in the grid.
- **Each page is searched on its own,** so several words must be on one page to match.

## When it looks right

These sign the four commits with your key, then put them on `main` and push. Do this with the branch checked out:

```bash
git rebase --exec "git commit --amend --no-edit -S" main
```

```bash
git switch main
```

```bash
git merge --ff-only overnight/m7-to-m9.5
```

```bash
git push origin main
```

Or tell me, and I'll do it once your key is unlocked. CI hasn't run on any of this, so it may find something the local runs didn't, as it did for the AppImage in M6. QtKeychain is new to the AppImage build in particular, and to the CI machines.
