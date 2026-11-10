#pragma once

#include "core/model/Publication.h"

#include <QString>
#include <QUrl>

#include <optional>

// What the app calls things on screen, and the checks on text and links that came from documents.
namespace papertrail::ui {

QString typeLabel(core::PublicationType type);
QString typeLabelFromDb(const QString &value);
QString readStatusLabel(core::ReadStatus status);
QString readStatusLabelFromDb(const QString &value);
// "4 stars", or "Not rated" for 0.
QString ratingLabel(int rating);
// "★★★★☆", or empty for 0.
QString ratingStars(int rating);
// What's offered with the read statuses, for publications opened in the last kRecentlyOpenedMs.
QString recentlyOpenedLabel();
QString fileStatusLabel(const QString &status); // Empty for a file with nothing wrong.
// What the bar across a retracted or withdrawn publication's cover says. Empty for anything else.
QString coverNoticeLabel(const QString &notice);
// What a notice worth warning about says of its publication, such as "Retracted in 2026" or "Concerns raised".
QString noticeWarningLabel(const QString &kind, const QString &date);
QString tabLabel(core::LibraryTab tab);

// A journal issue's numbers and date, such as "Vol. 7, No. 11, Nov 2026". Empty if there are none.
QString issueNumbering(const QString &volume, const QString &issue, std::optional<int> month, std::optional<int> year);

// For a button leading from an article to what holds it, such as "Show the issue".
QString showHolderLabel(core::PublicationType holderType);
QString publicationCount(qint64 count); // "1 publication", "1,200 publications"

// Metadata comes from files and online sources, so a link in it can't be trusted. Only web
// addresses may be opened. Returns an empty URL for anything else, such as file: or a custom scheme.
QUrl safeWebLink(const QString &text);
QUrl doiLink(const QString &doi);

// Puts text into a rich-text label so it can only ever appear as text.
QString linkHtml(const QUrl &url, const QString &text);

} // namespace papertrail::ui
