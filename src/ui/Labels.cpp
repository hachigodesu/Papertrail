#include "ui/Labels.h"

#include <QCoreApplication>
#include <QLocale>

namespace papertrail::ui {

using namespace core;

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Labels", text);
}

} // namespace

QString typeLabel(PublicationType type)
{
    switch (type) {
    case PublicationType::JournalIssue: return tr("Journal issue");
    case PublicationType::JournalArticle: return tr("Journal article");
    case PublicationType::ConferenceProceedings: return tr("Conference proceedings");
    case PublicationType::ConferencePaper: return tr("Conference paper");
    case PublicationType::Textbook: return tr("Textbook");
    case PublicationType::Book: return tr("Book");
    case PublicationType::BookChapter: return tr("Book chapter");
    case PublicationType::Thesis: return tr("Thesis");
    case PublicationType::Dissertation: return tr("Dissertation");
    case PublicationType::TechnicalReport: return tr("Technical report");
    case PublicationType::Standard: return tr("Standard");
    case PublicationType::Preprint: return tr("Preprint");
    case PublicationType::Other: return tr("Other");
    }
    return tr("Other");
}

QString typeLabelFromDb(const QString &value)
{
    const auto type = publicationTypeFromDb(value);
    return type ? typeLabel(*type) : value;
}

QString readStatusLabel(ReadStatus status)
{
    switch (status) {
    case ReadStatus::Unread: return tr("Unread");
    case ReadStatus::Reading: return tr("Reading");
    case ReadStatus::Read: return tr("Read");
    }
    return tr("Unread");
}

QString readStatusLabelFromDb(const QString &value)
{
    const auto status = readStatusFromDb(value);
    return status ? readStatusLabel(*status) : value;
}

QString fileStatusLabel(const QString &status)
{
    if (status == QLatin1StringView("missing"))
        return tr("File missing");
    if (status == QLatin1StringView("offline"))
        return tr("Drive offline");
    if (status == QLatin1StringView("unreadable"))
        return tr("Can't be read");
    return {};
}

QString issueNumbering(const QString &volume, const QString &issue, std::optional<int> month, std::optional<int> year)
{
    QStringList parts;
    if (!volume.isEmpty())
        parts << tr("Vol. %1").arg(volume);
    if (!issue.isEmpty())
        parts << tr("No. %1").arg(issue);
    if (year) {
        parts << (month && *month >= 1 && *month <= 12
                      ? QLocale().monthName(*month, QLocale::ShortFormat) + u' ' + QString::number(*year)
                      : QString::number(*year));
    }
    return parts.join(QStringLiteral(", "));
}

QString showHolderLabel(PublicationType holderType)
{
    switch (holderType) {
    case PublicationType::JournalIssue: return tr("Show the issue");
    case PublicationType::ConferenceProceedings: return tr("Show the proceedings");
    case PublicationType::Book:
    case PublicationType::Textbook: return tr("Show the book");
    default: return tr("Show what it's part of");
    }
}

QString tabLabel(LibraryTab tab)
{
    switch (tab) {
    case LibraryTab::All: return tr("All");
    case LibraryTab::JournalIssues: return tr("Journal Issues");
    case LibraryTab::JournalArticles: return tr("Journal Articles");
    case LibraryTab::Preprints: return tr("Preprints");
    case LibraryTab::ConferencePapers: return tr("Conference Papers");
    case LibraryTab::Textbooks: return tr("Textbooks");
    case LibraryTab::ThesesAndDissertations: return tr("Theses && Dissertations");
    case LibraryTab::TechnicalReports: return tr("Technical Reports");
    case LibraryTab::Standards: return tr("Standards");
    case LibraryTab::Other: return tr("Other");
    }
    return {};
}

QString publicationCount(qint64 count)
{
    return count == 1 ? tr("1 publication") : tr("%1 publications").arg(QLocale().toString(count));
}

QUrl safeWebLink(const QString &text)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1StringView("http") || scheme == QLatin1StringView("https") ? url : QUrl();
}

QUrl doiLink(const QString &doi)
{
    if (doi.isEmpty())
        return {};
    QUrl url(QStringLiteral("https://doi.org/"));
    // Encoded as a single path, so a DOI can't change where the link goes.
    url.setPath(QStringLiteral("/") + doi, QUrl::DecodedMode);
    return url;
}

QString linkHtml(const QUrl &url, const QString &text)
{
    return QStringLiteral("<a href=\"%1\">%2</a>")
        .arg(QString::fromUtf8(url.toEncoded()).toHtmlEscaped(), text.toHtmlEscaped());
}

} // namespace papertrail::ui
