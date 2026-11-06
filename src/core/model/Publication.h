#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {

enum class PublicationType {
    JournalVolume,
    JournalArticle,
    ConferenceProceedings,
    ConferencePaper,
    Textbook,
    Book,
    BookChapter,
    Thesis,
    Dissertation,
    TechnicalReport,
    Standard,
    Preprint, // Only ever chosen by the user. See import::classify.
    Other,
};

const QList<PublicationType> &allPublicationTypes();
QString toDbString(PublicationType type);
std::optional<PublicationType> publicationTypeFromDb(const QString &value);

// The library's tabs. Each shows one or more publication types.
enum class LibraryTab {
    All,
    JournalVolumes,
    JournalArticles,
    Preprints,
    ConferencePapers,
    Textbooks,
    ThesesAndDissertations,
    TechnicalReports,
    Standards,
    Other,
};

// The types a tab shows. Empty for LibraryTab::All, which applies no restriction.
QList<PublicationType> typesForTab(LibraryTab tab);

enum class ReadStatus { Unread, Reading, Read };

QString toDbString(ReadStatus status);
std::optional<ReadStatus> readStatusFromDb(const QString &value);

enum class PersonRole { Author, Editor, Translator, Advisor };

QString toDbString(PersonRole role);

struct PersonName {
    QString family;
    QString given;

    QString display() const; // "Family, Given", or just "Family".
    bool operator==(const PersonName &) const = default;
};

// A full publication record. Text fields use a null QString for "not set".
struct Publication {
    qint64 id = 0;
    QString uuid;
    PublicationType type = PublicationType::Other;

    std::optional<qint64> parentId;
    std::optional<int> pageStart;
    std::optional<int> pageEnd;

    QString title;
    QString subtitle;
    QString abstract;
    std::optional<int> year;
    std::optional<int> month;
    std::optional<int> day;
    QString language;
    QString url;
    QString edition;
    QString series;

    QString doi;
    QString isbn10;
    QString isbn13;
    QString issn;
    QString eissn;
    QString arxivId;
    QString pmid;

    QString containerTitle;
    QString volume;
    QString issue;
    QString pages;
    QString publisher;
    QString place;

    QString conferenceName;
    QString conferenceLocation;

    QString institution;
    QString department;
    QString degree;

    QString reportNumber;

    QString standardNumber;
    QString standardBody;
    QString standardRevision;
    QString standardStatus;

    ReadStatus readStatus = ReadStatus::Unread;
    std::optional<int> rating;
    QString notes;
    QString extraJson;

    bool needsReview = false;

    QList<PersonName> authors;
    QList<PersonName> editors;
    QList<PersonName> translators;
    QList<PersonName> advisors;

    qint64 createdAt = 0;
    qint64 updatedAt = 0;
};

// What a library list needs for one row.
struct PublicationSummary {
    qint64 id = 0;
    PublicationType type = PublicationType::Other;
    QString title;
    QString authors;
    std::optional<int> year;
    QString containerTitle;
    ReadStatus readStatus = ReadStatus::Unread;
    bool needsReview = false;

    // From the publication's main file, or its volume's. All null if it has no file.
    QString coverSha;
    QString format;
    QString fileStatus; // "ok", "missing", "offline" or "unreadable".
    QStringList formats; // Every format it's available in, starting with the main file's.
};

} // namespace papertrail::core
