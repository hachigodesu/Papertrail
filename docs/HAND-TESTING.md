# Testing M11.5 by hand

M11.5 was built and tested automatically on Windows, Arch and Ubuntu, with nobody using it. This list is what only a person can check, most important first. Details are in `docs/M11.5-results.md`.

Nothing from M11.5 is committed yet.

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

**Opening your library with this build upgrades it** (tables for removed files, nested collections, searching notes, retraction notices and batches). It makes a backup first, in a `backups` folder beside the library, so you have a way back without copying it yourself.

## 1. Backups

1. After opening, check **File > Backups...** lists a backup from before the upgrade, and a daily one about 10 seconds later.
2. Choose **Back up now**, change a publication's title, then restore that backup. The title should go back, and a "Before a backup was restored" one should appear, which undoes it if restored.
3. Untick the daily backups, and tick them again.

## 2. Removing publications

1. Choose a publication and press **Delete**. Read the question, and remove it.
2. Choose **Scan again** in **Library folders**. It shouldn't come back.
3. In **Library folders**, choose **Bring back...**, bring it back, and check it returns after the scan.

## 3. Dragging, and collections inside collections

1. Choose several covers, then press on one of them and drag onto a collection in the sidebar. Then onto a tag.
2. Check that pressing on a cover that isn't chosen and dragging still chooses, including a short last row.
3. Right-click a collection, make one inside it, and use **Move...**. Choosing the outer one should show what's in both.
4. Add an issue with articles to a collection, and check its articles are in it too.

## 4. Highlights and notes

1. Search `note:` and a word from one of your highlights or notes, then the word on its own.
2. Open **View > Highlights and notes...** (Ctrl+Shift+N), search it, and open one. It should go to its page.

## 5. Retractions

With online lookups on, leave Papertrail open for a few minutes. If anything in your library has been retracted, **Retracted or of concern** appears in the filters, and its details say so near the top. If nothing has, that's the expected result. **File > Online lookups...** has the setting to turn checking off.

## 6. An issue without contents

If you have an issue with no table of contents, choose **Create articles from contents...** on it. Check what it proposes: the titles, the page ranges, and that the issue's own pages aren't offered as articles. Nothing is made until you choose to.

## 7. A copy with highlights

Right-click a PDF you've highlighted, choose **Save a copy with highlights...**, and open the copy in another PDF reader, such as a web browser or Acrobat. Check the highlights, underlines and notes are where they should be, and that the original is unchanged.

## 8. Summaries at half price

This costs a little, so only if you're willing:

1. Choose two publications, one with a summary already, and choose **Summarize 2 later at half price...**. Read the dialog and its estimate, and send.
2. Check the details panel says the summary is on its way, and the status bar says how many. Click either to open **Summaries from Claude**.
3. Close Papertrail for a while, then open it again. Within the hour, usually, the status bar should say a summary is waiting to be reviewed.
4. Review it under **To review**, and keep it. It should then be in the details panel.
5. Check **Spending** shows what it cost, along with any summary you asked for straight away.

The batch you sent before this change carries over. If it hadn't arrived, it's listed as on its way. If it had, it's already the publication's summary.

## Things I decided that you may want to change

- **Batches never send the paper,** only identifying details, since sending a paper needs its own confirmation.
- **Retraction checks are on by default** when lookups are online.
- **Collection names are unique across the library,** not just among those in the same collection.
- **The suggested place for a highlighted copy is Documents,** since a copy in a library folder would be added as a publication of its own.
- **Deleting a collection deletes the ones inside it,** as with tags.

## When it looks right

Tell me, and I'll commit M11.5 as one signed commit, and push when you say.
