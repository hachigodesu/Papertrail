#include "core/model/Publication.h"

namespace papertrail::core {

const QList<PublicationType> &allPublicationTypes()
{
    static const QList<PublicationType> types = {
        PublicationType::JournalVolume, PublicationType::JournalArticle,
        PublicationType::ConferenceProceedings, PublicationType::ConferencePaper,
        PublicationType::Textbook, PublicationType::Book, PublicationType::BookChapter,
        PublicationType::Thesis, PublicationType::Dissertation, PublicationType::TechnicalReport,
        PublicationType::Standard, PublicationType::Preprint, PublicationType::Other,
    };
    return types;
}

QString toDbString(PublicationType type)
{
    switch (type) {
    case PublicationType::JournalVolume: return QStringLiteral("journal_volume");
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
    case LibraryTab::JournalVolumes:
        return {PublicationType::JournalVolume};
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
