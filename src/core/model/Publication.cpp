#include "core/model/Publication.h"

namespace papertrail::core {

const QList<PublicationType> &allPublicationTypes()
{
    static const QList<PublicationType> types = {
        PublicationType::JournalIssue, PublicationType::JournalArticle,
        PublicationType::ConferenceProceedings, PublicationType::ConferencePaper,
        PublicationType::Textbook, PublicationType::Book, PublicationType::BookChapter,
        PublicationType::Thesis, PublicationType::Dissertation, PublicationType::TechnicalReport,
        PublicationType::Standard, PublicationType::Preprint, PublicationType::Other,
    };
    return types;
}

QString articleCoverKey(const QString &sha256, int firstPage)
{
    if (sha256.isEmpty() || firstPage < 1)
        return sha256;
    return sha256 + u'#' + QString::number(firstPage);
}

QString coverKeyHash(const QString &key)
{
    const qsizetype mark = key.indexOf(u'#');
    return mark < 0 ? key : key.left(mark);
}

int coverKeyPage(const QString &key)
{
    const qsizetype mark = key.indexOf(u'#');
    return mark < 0 ? 0 : key.mid(mark + 1).toInt();
}

QString toDbString(PublicationType type)
{
    switch (type) {
    // Stored under its first name, since the type check in the schema can't be changed without rebuilding
    // the table. A volume holds several issues, so it's called an issue everywhere else.
    case PublicationType::JournalIssue: return QStringLiteral("journal_volume");
    case PublicationType::JournalArticle: return QStringLiteral("journal_article");
    case PublicationType::ConferenceProceedings: return QStringLiteral("conference_proceedings");
    case PublicationType::ConferencePaper: return QStringLiteral("conference_paper");
    case PublicationType::Textbook: return QStringLiteral("textbook");
    case PublicationType::Book: return QStringLiteral("book");
    case PublicationType::BookChapter: return QStringLiteral("book_chapter");
    case PublicationType::Thesis: return QStringLiteral("thesis");
    case PublicationType::Dissertation: return QStringLiteral("dissertation");
    case PublicationType::TechnicalReport: return QStringLiteral("technical_report");
    case PublicationType::Standard: return QStringLiteral("standard");
    case PublicationType::Preprint: return QStringLiteral("preprint");
    case PublicationType::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

std::optional<PublicationType> publicationTypeFromDb(const QString &value)
{
    for (const PublicationType type : allPublicationTypes()) {
        if (toDbString(type) == value)
            return type;
    }
    return std::nullopt;
}

QList<PublicationType> typesForTab(LibraryTab tab)
{
    switch (tab) {
    case LibraryTab::All:
        return {};
    case LibraryTab::JournalIssues:
        return {PublicationType::JournalIssue};
    case LibraryTab::JournalArticles:
        return {PublicationType::JournalArticle};
    case LibraryTab::Preprints:
        return {PublicationType::Preprint};
    case LibraryTab::ConferencePapers:
        return {PublicationType::ConferenceProceedings, PublicationType::ConferencePaper};
    case LibraryTab::Textbooks:
        return {PublicationType::Textbook, PublicationType::Book, PublicationType::BookChapter};
    case LibraryTab::ThesesAndDissertations:
        return {PublicationType::Thesis, PublicationType::Dissertation};
    case LibraryTab::TechnicalReports:
        return {PublicationType::TechnicalReport};
    case LibraryTab::Standards:
        return {PublicationType::Standard};
    case LibraryTab::Other:
        return {PublicationType::Other};
    }
    return {};
}

QString toDbString(ReadStatus status)
{
    switch (status) {
    case ReadStatus::Unread: return QStringLiteral("unread");
    case ReadStatus::Reading: return QStringLiteral("reading");
    case ReadStatus::Read: return QStringLiteral("read");
    }
    return QStringLiteral("unread");
}

std::optional<ReadStatus> readStatusFromDb(const QString &value)
{
    for (const ReadStatus status : {ReadStatus::Unread, ReadStatus::Reading, ReadStatus::Read}) {
        if (toDbString(status) == value)
            return status;
    }
    return std::nullopt;
}

QString toDbString(PersonRole role)
{
    switch (role) {
    case PersonRole::Author: return QStringLiteral("author");
    case PersonRole::Editor: return QStringLiteral("editor");
    case PersonRole::Translator: return QStringLiteral("translator");
    case PersonRole::Advisor: return QStringLiteral("advisor");
    }
    return QStringLiteral("author");
}

QString PersonName::display() const
{
    return given.isEmpty() ? family : family + QStringLiteral(", ") + given;
}

} // namespace papertrail::core
