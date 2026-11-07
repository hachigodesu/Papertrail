#include "metadata/Matching.h"

#include "core/db/Database.h"
#include "core/model/Identifiers.h"
#include "core/model/PublicationFields.h"
#include "core/repo/LookupRepository.h"
#include "core/repo/PublicationRepository.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace papertrail::metadata {

using namespace core;

namespace {

QHash<QString, int> wordCounts(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[^\\p{L}\\p{N}]+"));
    QHash<QString, int> counts;
    const QStringList words = titleSortKey(text).split(separators, Qt::SkipEmptyParts);
    for (const QString &word : words)
        ++counts[word];
    return counts;
}

double wordOverlap(const QString &a, const QString &b)
{
    const QHash<QString, int> wordsA = wordCounts(a);
    const QHash<QString, int> wordsB = wordCounts(b);
    int totalA = 0;
    for (const int count : wordsA)
        totalA += count;
    int totalB = 0;
    for (const int count : wordsB)
        totalB += count;
    if (totalA == 0 || totalB == 0)
        return 0;
    int shared = 0;
    for (auto it = wordsA.cbegin(); it != wordsA.cend(); ++it)
        shared += qMin(it.value(), wordsB.value(it.key()));
    return 2.0 * shared / (totalA + totalB);
}

// A source, and the fields that came from it, count as lookup changes that can be undone.
bool isLookupSource(const QString &source)
{
    return source == kCrossref || source == kOpenAlex || source == kArxiv || source == kOpenLibrary;
}

} // namespace

double titleSimilarity(const QString &a, const QString &b)
{
    const QString mainA = a.section(u':', 0, 0);
    const QString mainB = b.section(u':', 0, 0);
    return qMax(wordOverlap(a, b), qMax(wordOverlap(mainA, b), qMax(wordOverlap(a, mainB), wordOverlap(mainA, mainB))));
}

double matchScore(const Publication &publication, const FoundRecord &found)
{
    double score = titleSimilarity(publication.title, found.publication.title);
    if (score < 0.5)
        return score * 0.8; // The rest can't rescue a title that's clearly different.

    double weight = 1.0;
    double total = score;
    if (!publication.authors.isEmpty() && !found.publication.authors.isEmpty()) {
        const bool same = publication.authors.first().family.compare(found.publication.authors.first().family, Qt::CaseInsensitive) == 0;
        total += same ? 0.4 : 0.0;
        weight += 0.4;
    }
    if (publication.year && found.publication.year) {
        const int apart = qAbs(*publication.year - *found.publication.year);
        total += apart == 0 ? 0.2 : apart == 1 ? 0.1 : 0.0;
        weight += 0.2;
    }
    return total / weight;
}

bool titleAgrees(const Publication &publication, const QStringList &fileNames, const FoundRecord &found)
{
    constexpr double kAgrees = 0.6;
    if (titleSimilarity(publication.title, found.publication.title) >= kAgrees)
        return true;
    return std::any_of(fileNames.cbegin(), fileNames.cend(), [&](const QString &name) {
        return titleSimilarity(QFileInfo(name).completeBaseName(), found.publication.title) >= kAgrees;
    });
}

QList<FieldChange> plannedChanges(const Publication &publication, const FoundRecord &found, const QSet<QString> &setByHand)
{
    // Better from the source than from a file, so these replace what's there.
    static const QSet<QString> replaced = {
        QStringLiteral("title"),     QStringLiteral("subtitle"),        QStringLiteral("authors"), QStringLiteral("editors"),
        QStringLiteral("year"),      QStringLiteral("month"),           QStringLiteral("day"),     QStringLiteral("container_title"),
        QStringLiteral("volume"),    QStringLiteral("issue"),           QStringLiteral("pages"),   QStringLiteral("publisher"),
        QStringLiteral("conference_name"), QStringLiteral("conference_location"),
    };

    QList<FieldChange> changes;
    for (const QString &name : metadataFieldNames()) {
        if (name == QLatin1StringView("type") || setByHand.contains(name))
            continue;
        const QString after = metadataFieldValue(found.publication, name);
        const QString before = metadataFieldValue(publication, name);
        if (after.isEmpty() || after == before)
            continue;
        if (before.isEmpty() || replaced.contains(name))
            changes << FieldChange{name, before, after};
    }

    // A type moves only between groups, because a textbook is more exact than the "book" a source
    // says. Preprints are left out both ways: they're only ever what the user has said.
    if (found.type && !setByHand.contains(QStringLiteral("type")) && publication.type != PublicationType::Preprint
        && *found.type != PublicationType::Preprint) {
        const QList<LibraryTab> tabs = {LibraryTab::JournalIssues, LibraryTab::JournalArticles, LibraryTab::ConferencePapers,
                                        LibraryTab::Textbooks, LibraryTab::ThesesAndDissertations, LibraryTab::TechnicalReports,
                                        LibraryTab::Standards, LibraryTab::Other};
        const bool sameGroup = std::any_of(tabs.cbegin(), tabs.cend(), [&](LibraryTab tab) {
            const QList<PublicationType> types = typesForTab(tab);
            return types.contains(publication.type) && types.contains(*found.type);
        });
        if (!sameGroup)
            changes << FieldChange{QStringLiteral("type"), toDbString(publication.type), toDbString(*found.type)};
    }
    return changes;
}

QSet<QString> fieldsSetByHand(Database &db, qint64 publicationId)
{
    QSet<QString> fields;
    auto select = db.prepare("SELECT DISTINCT field FROM metadata_provenance WHERE publication_id = ? AND source = 'manual'");
    select->bind(1, publicationId);
    while (select->step())
        fields.insert(select->textAt(0));
    return fields;
}

int applyFoundRecord(Database &db, qint64 publicationId, const FoundRecord &found, bool needsReview)
{
    PublicationRepository publications(db);
    Transaction transaction(db);
    std::optional<Publication> publication = publications.get(publicationId);
    if (!publication)
        return 0;

    const QList<FieldChange> changes = plannedChanges(*publication, found, fieldsSetByHand(db, publicationId));
    for (const FieldChange &change : changes)
        setMetadataFieldValue(*publication, change.field, change.after);
    publication->needsReview = needsReview;

    QJsonObject extra = QJsonDocument::fromJson(publication->extraJson.toUtf8()).object();
    if (found.preprintHint)
        extra.insert(QStringLiteral("preprintHint"), found.provider);
    else
        extra.remove(QStringLiteral("preprintHint"));
    publication->extraJson = extra.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact));

    // One time for all of them, so that undoing can tell which changes went together.
    publications.updateWithProvenance(*publication, found.provider, QDateTime::currentMSecsSinceEpoch());
    transaction.commit();
    return int(changes.size());
}

QString undoableSource(Database &db, qint64 publicationId)
{
    auto select = db.prepare(
        "SELECT source FROM metadata_provenance WHERE publication_id = ? AND source IN ('crossref', 'openalex', 'arxiv', 'openlibrary') "
        "AND recorded_at > coalesce((SELECT max(recorded_at) FROM metadata_provenance WHERE publication_id = ?1 AND source = 'undo'), 0) "
        "ORDER BY recorded_at DESC, id DESC LIMIT 1");
    select->bind(1, publicationId);
    return select->step() ? select->textAt(0) : QString();
}

int undoLastLookup(Database &db, qint64 publicationId)
{
    PublicationRepository publications(db);
    Transaction transaction(db);
    std::optional<Publication> publication = publications.get(publicationId);
    const QString source = undoableSource(db, publicationId);
    if (!publication || !isLookupSource(source))
        return 0;

    auto when = db.prepare(
        "SELECT max(recorded_at) FROM metadata_provenance WHERE publication_id = ? AND source = ?");
    when->bind(1, publicationId);
    when->bind(2, source);
    when->step();
    const qint64 batch = when->int64At(0);

    int restored = 0;
    {
        auto entries = db.prepare(
            "SELECT field, previous_value FROM metadata_provenance WHERE publication_id = ? AND source = ? AND recorded_at = ?");
        entries->bind(1, publicationId);
        entries->bind(2, source);
        entries->bind(3, batch);
        while (entries->step()) {
            const QString field = entries->textAt(0);
            // Something changed by hand since then is left as it is.
            if (metadataFieldNames().contains(field) && !fieldsSetByHand(db, publicationId).contains(field)) {
                setMetadataFieldValue(*publication, field, entries->textAt(1));
                ++restored;
            }
        }
    }

    QJsonObject extra = QJsonDocument::fromJson(publication->extraJson.toUtf8()).object();
    extra.remove(QStringLiteral("preprintHint"));
    publication->extraJson = extra.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact));

    publications.updateWithProvenance(*publication, QStringLiteral("undo"), QDateTime::currentMSecsSinceEpoch() + 1);
    transaction.commit();
    return restored;
}

QString providerLabel(const QString &provider)
{
    if (provider == kCrossref)
        return QStringLiteral("Crossref");
    if (provider == kOpenAlex)
        return QStringLiteral("OpenAlex");
    if (provider == kArxiv)
        return QStringLiteral("arXiv");
    if (provider == kOpenLibrary)
        return QStringLiteral("Open Library");
    return provider;
}

} // namespace papertrail::metadata
