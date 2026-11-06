#pragma once

#include "core/model/Publication.h"

#include <QString>
#include <QUrl>

// What the app calls things on screen, and the checks on text and links that came from documents.
namespace papertrail::ui {

QString typeLabel(core::PublicationType type);
QString typeLabelFromDb(const QString &value);
QString readStatusLabel(core::ReadStatus status);
QString readStatusLabelFromDb(const QString &value);
QString fileStatusLabel(const QString &status); // Empty for a file with nothing wrong.
QString tabLabel(core::LibraryTab tab);
QString publicationCount(qint64 count); // "1 publication", "1,200 publications"

// Metadata comes from files and online sources, so a link in it can't be trusted. Only web
// addresses may be opened. Returns an empty URL for anything else, such as file: or a custom scheme.
QUrl safeWebLink(const QString &text);
QUrl doiLink(const QString &doi);

// Puts text into a rich-text label so it can only ever appear as text.
QString linkHtml(const QUrl &url, const QString &text);

} // namespace papertrail::ui
