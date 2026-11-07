#pragma once

#include "core/model/Publication.h"

#include <QString>

#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::import {

// A title an issue got from its file that only describes it, such as its cover's "OCTOBER 2026 VOLUME 18
// NUMBER 5" or the "Publication Information" page's title, or just its journal's name.
bool isDescriptiveIssueTitle(const QString &title, const QString &journal);

// A title made from an issue's details, such as "IEEE Transactions on Artificial Intelligence, Vol. 7, No. 11
// (November 2026)". Empty if its journal isn't known.
QString issueTitle(const core::Publication &issue);

// Puts an issue's details in order, as far as they're known:
// - its volume, issue, date and ISSN are read from its title where they're empty
// - its journal, ISSN and publisher come from `article` where it has none, such as after an article's lookup
// - a title that only describes it becomes one made from its details, and articles that took that title as
//   their journal's name get the journal's real name
// Nothing set by hand is changed, and each change can be undone. Returns whether anything changed.
bool tidyJournalIssue(core::Database &db, qint64 issueId, const std::optional<core::Publication> &article = std::nullopt);

// The same for every issue in the library, such as when a library is opened.
int tidyJournalIssues(core::Database &db);

} // namespace papertrail::import
