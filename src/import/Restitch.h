#pragma once

// Changing the order a book's parts are stitched in, for a book downloaded as a zip of PDFs.

#include "import/Stitching.h"

#include <QList>
#include <QString>

namespace papertrail::core {
class Database;
}

namespace papertrail::import {

struct RestitchCounts {
    int moved = 0;       // Highlights, notes and bookmarks now on other pages.
    int setAside = 0;    // Those on parts now left out, kept to come back if the parts are put back.
    int broughtBack = 0; // Those set aside earlier, back on parts put back.
};

// How many highlights, notes and bookmarks on this zip's contents are on parts `to` leaves out.
int marksLeftOut(core::Database &db, const QString &zipSha256, const QList<OrderedPart> &to);

// Keeps `to` as the zip's order, and moves everything kept by page with its part and its page within that part:
// highlights, notes, bookmarks, the reading place of each publication with this zip, and the pages of the chapters
// inside them. Those on parts left out are set aside, and those set aside on parts put back come back. All in one
// transaction. What was read from the old stitched PDF, its text and recognised words, is the caller's to forget.
RestitchCounts applyStitchOrder(core::Database &db, const QString &zipSha256, const QList<OrderedPart> &to, bool confirmed);

// The publications whose files have this zip's contents.
QList<qint64> publicationsWithContents(core::Database &db, const QString &zipSha256);

} // namespace papertrail::import
