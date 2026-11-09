#include "transfer/LibraryTransfer.h"

#include "core/db/Database.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationMerge.h"
#include "core/repo/PublicationRepository.h"
#include "transfer/Zip.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

#include <stdexcept>

namespace papertrail::transfer {

using core::Database;
using core::FileRecord;
using core::FileRepository;
using core::PersonName;
using core::Publication;
using core::PublicationRepository;
using core::PublicationType;
using core::Statement;
using core::Transaction;

QString bundleSuffix()
{
    return QStringLiteral("papertrail");
}

namespace {

constexpr const char *kImportSource = "library import";

// ---------------------------------------------------------------------------------------------------- Fields

// Every text field of a publication, by the name it has in an export.
struct TextField {
    const char *key;
    QString Publication::*member;
};

const QList<TextField> &textFields()
{
    static const QList<TextField> fields = {
        {"title", &Publication::title},
        {"subtitle", &Publication::subtitle},
        {"abstract", &Publication::abstract},
        {"language", &Publication::language},
        {"url", &Publication::url},
        {"edition", &Publication::edition},
        {"series", &Publication::series},
        {"doi", &Publication::doi},
        {"isbn10", &Publication::isbn10},
        {"isbn13", &Publication::isbn13},
        {"issn", &Publication::issn},
        {"eissn", &Publication::eissn},
        {"arxivId", &Publication::arxivId},
        {"pmid", &Publication::pmid},
        {"containerTitle", &Publication::containerTitle},
        {"volume", &Publication::volume},
        {"issue", &Publication::issue},
        {"pages", &Publication::pages},
        {"publisher", &Publication::publisher},
        {"place", &Publication::place},
        {"conferenceName", &Publication::conferenceName},
        {"conferenceLocation", &Publication::conferenceLocation},
        {"institution", &Publication::institution},
        {"department", &Publication::department},
        {"degree", &Publication::degree},
        {"reportNumber", &Publication::reportNumber},
        {"standardNumber", &Publication::standardNumber},
        {"standardBody", &Publication::standardBody},
        {"standardRevision", &Publication::standardRevision},
        {"standardStatus", &Publication::standardStatus},
        {"notes", &Publication::notes},
        {"extraJson", &Publication::extraJson},
    };
    return fields;
}

struct PeopleField {
    const char *key;
    QList<PersonName> Publication::*member;
};

const QList<PeopleField> &peopleFields()
{
    static const QList<PeopleField> fields = {
        {"authors", &Publication::authors},
        {"editors", &Publication::editors},
        {"translators", &Publication::translators},
        {"advisors", &Publication::advisors},
    };
    return fields;
}

// Types by the names an export uses. A journal issue is stored under its first name, but exported as an issue.
QString exportTypeName(PublicationType type)
{
    return type == PublicationType::JournalIssue ? QStringLiteral("journal_issue") : core::toDbString(type);
}

PublicationType importTypeName(const QString &name)
{
    if (name == QLatin1StringView("journal_issue"))
        return PublicationType::JournalIssue;
    return core::publicationTypeFromDb(name).value_or(PublicationType::Other);
}

QJsonValue optionalInt(const std::optional<int> &value)
{
    return value ? QJsonValue(*value) : QJsonValue();
}

std::optional<int> intFrom(const QJsonValue &value)
{
    return value.isDouble() ? std::optional<int>(value.toInt()) : std::nullopt;
}

QJsonObject publicationToJson(const Publication &p, const QString &parentUuid)
{
    QJsonObject object;
    object.insert(QStringLiteral("uuid"), p.uuid);
    object.insert(QStringLiteral("type"), exportTypeName(p.type));
    if (!parentUuid.isEmpty()) {
        object.insert(QStringLiteral("parent"), parentUuid);
        object.insert(QStringLiteral("pageStart"), optionalInt(p.pageStart));
        object.insert(QStringLiteral("pageEnd"), optionalInt(p.pageEnd));
    }
    for (const TextField &field : textFields()) {
        const QString &value = p.*(field.member);
        if (!value.isNull())
            object.insert(QLatin1StringView(field.key), value);
    }
    object.insert(QStringLiteral("year"), optionalInt(p.year));
    object.insert(QStringLiteral("month"), optionalInt(p.month));
    object.insert(QStringLiteral("day"), optionalInt(p.day));
    object.insert(QStringLiteral("readStatus"), core::toDbString(p.readStatus));
    object.insert(QStringLiteral("rating"), optionalInt(p.rating));
    object.insert(QStringLiteral("needsReview"), p.needsReview);
    for (const PeopleField &field : peopleFields()) {
        QJsonArray people;
        for (const PersonName &person : p.*(field.member))
            people.append(QJsonObject{{QStringLiteral("family"), person.family}, {QStringLiteral("given"), person.given}});
        if (!people.isEmpty())
            object.insert(QLatin1StringView(field.key), people);
    }
    object.insert(QStringLiteral("createdAt"), double(p.createdAt));
    object.insert(QStringLiteral("updatedAt"), double(p.updatedAt));
    return object;
}

// Fills in a publication from an export's record. The id, uuid, parent and timestamps are left to the caller.
void publicationFromJson(Publication &p, const QJsonObject &object)
{
    p.type = importTypeName(object.value(QStringLiteral("type")).toString());
    for (const TextField &field : textFields()) {
        const QJsonValue value = object.value(QLatin1StringView(field.key));
        p.*(field.member) = value.isString() ? value.toString() : QString();
    }
    p.year = intFrom(object.value(QStringLiteral("year")));
    p.month = intFrom(object.value(QStringLiteral("month")));
    p.day = intFrom(object.value(QStringLiteral("day")));
    p.readStatus = core::readStatusFromDb(object.value(QStringLiteral("readStatus")).toString()).value_or(core::ReadStatus::Unread);
    p.rating = intFrom(object.value(QStringLiteral("rating")));
    p.needsReview = object.value(QStringLiteral("needsReview")).toBool();
    for (const PeopleField &field : peopleFields()) {
        QList<PersonName> people;
        for (const QJsonValue &value : object.value(QLatin1StringView(field.key)).toArray()) {
            const QJsonObject person = value.toObject();
            const QString family = person.value(QStringLiteral("family")).toString().trimmed();
            if (!family.isEmpty())
                people << PersonName{family, person.value(QStringLiteral("given")).toString().trimmed()};
        }
        p.*(field.member) = people;
    }
}

bool sameDetails(const Publication &a, const Publication &b)
{
    if (a.type != b.type || a.year != b.year || a.month != b.month || a.day != b.day || a.readStatus != b.readStatus
        || a.rating != b.rating || a.needsReview != b.needsReview)
        return false;
    for (const TextField &field : textFields()) {
        if ((a.*(field.member)).trimmed() != (b.*(field.member)).trimmed())
            return false;
    }
    for (const PeopleField &field : peopleFields()) {
        if (a.*(field.member) != b.*(field.member))
            return false;
    }
    return true;
}

QJsonArray rows(Database &db, const char *sql, const std::function<QJsonObject(Statement &)> &row)
{
    QJsonArray array;
    auto statement = db.prepare(sql);
    while (statement->step())
        array.append(row(*statement));
    return array;
}

QJsonValue textOrNull(const Statement &statement, int column)
{
    return statement.isNull(column) ? QJsonValue() : QJsonValue(statement.textAt(column));
}

QJsonValue numberOrNull(const Statement &statement, int column)
{
    return statement.isNull(column) ? QJsonValue() : QJsonValue(statement.doubleAt(column));
}

QString textValue(const QJsonValue &value)
{
    return value.isString() ? value.toString() : QString();
}

qint64 int64Value(const QJsonValue &value)
{
    return qint64(value.toDouble());
}

// Publications only kept for a removed folder, and the articles inside them, aren't part of the library. Those
// waiting for their files are hidden too, but they are part of it, and go with their files' fingerprints.
#define PAPERTRAIL_IN_LIBRARY(p)                                                                                         \
    "(" p ".id NOT IN (SELECT publication_id FROM hidden_publication) OR coalesce(" p ".parent_id, " p ".id) IN "       \
    "(SELECT publication_id FROM expected_file))"
const char *const kVisible = PAPERTRAIL_IN_LIBRARY("p");

} // namespace

// ---------------------------------------------------------------------------------------------------- Export

QByteArray buildBundle(Database &db, ExportReport &report, const Progress &progress)
{
    report = {};
    PublicationRepository publications(db);
    FileRepository files(db);
    const QSet<qint64> removedRoots = [&] {
        QSet<qint64> ids;
        for (const core::LibraryRoot &root : files.removedRoots())
            ids.insert(root.id);
        return ids;
    }();

    // Ids and uuids, so that links between records can be written as uuids.
    QHash<qint64, QString> publicationUuids;
    QList<qint64> ids;
    {
        auto select = db.prepare(QByteArray("SELECT p.id, p.uuid FROM publication p WHERE ") + kVisible
                                 + " ORDER BY p.parent_id IS NOT NULL, p.id");
        while (select->step()) {
            ids << select->int64At(0);
            publicationUuids.insert(select->int64At(0), select->textAt(1));
        }
    }
    QHash<qint64, QString> tagUuids;
    {
        auto select = db.prepare("SELECT id, uuid FROM tag");
        while (select->step())
            tagUuids.insert(select->int64At(0), select->textAt(1));
    }
    QHash<qint64, QStringList> tagsOf;
    {
        auto select = db.prepare("SELECT publication_id, tag_id FROM publication_tag ORDER BY publication_id, tag_id");
        while (select->step())
            tagsOf[select->int64At(0)] << tagUuids.value(select->int64At(1));
    }

    QJsonArray publicationArray;
    for (qsizetype i = 0; i < ids.size(); ++i) {
        if (progress.cancelled && progress.cancelled()) {
            report.cancelled = true;
            return {};
        }
        const std::optional<Publication> p = publications.get(ids.at(i));
        if (!p)
            continue;
        QJsonObject object = publicationToJson(*p, p->parentId ? publicationUuids.value(*p->parentId) : QString());
        const QStringList tags = tagsOf.value(p->id);
        if (!tags.isEmpty())
            object.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
        QJsonArray fileArray;
        QSet<QString> seen;
        for (const FileRecord &file : files.filesFor(p->id)) {
            // A copy in a removed folder isn't somewhere the publication is any more.
            if (file.sha256.isEmpty() || removedRoots.contains(file.rootId) || seen.contains(file.sha256))
                continue;
            seen.insert(file.sha256);
            const QString name = file.relativePath.section(u'/', -1);
            fileArray.append(QJsonObject{{QStringLiteral("sha256"), file.sha256},
                                         {QStringLiteral("size"), double(file.size)},
                                         {QStringLiteral("format"), file.format},
                                         {QStringLiteral("name"), name},
                                         {QStringLiteral("primary"), fileArray.isEmpty()}});
        }
        Statement &expected = db.cached(
            "SELECT sha256, size, format, file_name, is_primary FROM expected_file WHERE publication_id = ? ORDER BY is_primary DESC");
        expected.bind(1, p->id);
        while (expected.step()) {
            if (seen.contains(expected.textAt(0)))
                continue;
            seen.insert(expected.textAt(0));
            fileArray.append(QJsonObject{{QStringLiteral("sha256"), expected.textAt(0)},
                                         {QStringLiteral("size"), double(expected.int64At(1))},
                                         {QStringLiteral("format"), expected.textAt(2)},
                                         {QStringLiteral("name"), expected.textAt(3)},
                                         {QStringLiteral("primary"), expected.intAt(4) != 0}});
        }
        expected.reset();
        if (!fileArray.isEmpty())
            object.insert(QStringLiteral("files"), fileArray);
        publicationArray.append(object);
        if (progress.progress && (i % 200 == 0 || i + 1 == ids.size()))
            progress.progress(i + 1, ids.size());
    }
    report.publications = publicationArray.size();

    const QJsonArray tagArray = rows(db, "SELECT t.uuid, t.name, p.uuid, t.colour, t.created_at FROM tag t "
                                         "LEFT JOIN tag p ON p.id = t.parent_id ORDER BY t.parent_id IS NOT NULL, t.id",
                                     [](Statement &s) {
                                         return QJsonObject{{QStringLiteral("uuid"), s.textAt(0)},
                                                            {QStringLiteral("name"), s.textAt(1)},
                                                            {QStringLiteral("parent"), textOrNull(s, 2)},
                                                            {QStringLiteral("colour"), textOrNull(s, 3)},
                                                            {QStringLiteral("createdAt"), double(s.int64At(4))}};
                                     });
    report.tags = tagArray.size();

    QJsonArray collectionArray;
    {
        auto select = db.prepare("SELECT c.id, c.uuid, c.name, c.created_at, c.updated_at, parent.uuid FROM collection c "
                                 "LEFT JOIN collection_parent cp ON cp.collection_id = c.id "
                                 "LEFT JOIN collection parent ON parent.id = cp.parent_id ORDER BY c.id");
        auto members = db.prepare(QByteArray("SELECT p.uuid FROM collection_publication c JOIN publication p ON p.id = c.publication_id "
                                             "WHERE c.collection_id = ? AND ")
                                  + kVisible + " ORDER BY c.added_at, p.id");
        while (select->step()) {
            QJsonArray uuids;
            members->bind(1, select->int64At(0));
            while (members->step())
                uuids.append(members->textAt(0));
            members->reset();
            QJsonObject entry{{QStringLiteral("uuid"), select->textAt(1)},
                              {QStringLiteral("name"), select->textAt(2)},
                              {QStringLiteral("createdAt"), double(select->int64At(3))},
                              {QStringLiteral("updatedAt"), double(select->int64At(4))},
                              {QStringLiteral("publications"), uuids}};
            if (!select->isNull(5))
                entry.insert(QStringLiteral("parentUuid"), select->textAt(5));
            collectionArray.append(entry);
        }
    }
    report.collections = collectionArray.size();

    // A saved search refers to tags, collections and people by what names them in any library, so it's copied as it is.
    QJsonArray savedSearchArray;
    {
        auto select = db.prepare("SELECT uuid, name, definition, created_at FROM saved_search ORDER BY id");
        while (select->step()) {
            savedSearchArray.append(QJsonObject{{QStringLiteral("uuid"), select->textAt(0)},
                                                {QStringLiteral("name"), select->textAt(1)},
                                                {QStringLiteral("definition"), QJsonDocument::fromJson(select->textAt(2).toUtf8()).object()},
                                                {QStringLiteral("createdAt"), double(select->int64At(3))}});
        }
    }

    const QJsonArray annotationArray = rows(
        db,
        "SELECT a.uuid, p.uuid, a.content_hash, a.location_json, a.type, a.colour, a.selected_text, a.note, a.needs_review, "
        "a.created_at, a.updated_at FROM annotation a JOIN publication p ON p.id = a.publication_id "
        "WHERE " PAPERTRAIL_IN_LIBRARY("p") " ORDER BY a.id",
        [](Statement &s) {
            return QJsonObject{{QStringLiteral("uuid"), s.textAt(0)},
                               {QStringLiteral("publication"), s.textAt(1)},
                               {QStringLiteral("contentHash"), textOrNull(s, 2)},
                               {QStringLiteral("location"), s.textAt(3)},
                               {QStringLiteral("type"), s.textAt(4)},
                               {QStringLiteral("colour"), textOrNull(s, 5)},
                               {QStringLiteral("selectedText"), textOrNull(s, 6)},
                               {QStringLiteral("note"), textOrNull(s, 7)},
                               {QStringLiteral("needsReview"), s.intAt(8) != 0},
                               {QStringLiteral("createdAt"), double(s.int64At(9))},
                               {QStringLiteral("updatedAt"), double(s.int64At(10))}};
        });
    report.annotations = annotationArray.size();

    const QJsonArray readingArray = rows(
        db,
        "SELECT p.uuid, r.location, r.zoom, r.last_opened FROM reading_state r JOIN publication p ON p.id = r.publication_id "
        "WHERE " PAPERTRAIL_IN_LIBRARY("p") " ORDER BY p.id",
        [](Statement &s) {
            return QJsonObject{{QStringLiteral("publication"), s.textAt(0)},
                               {QStringLiteral("location"), textOrNull(s, 1)},
                               {QStringLiteral("zoom"), numberOrNull(s, 2)},
                               {QStringLiteral("lastOpened"), numberOrNull(s, 3)}};
        });

    const QJsonArray provenanceArray = rows(
        db,
        "SELECT p.uuid, m.field, m.source, m.confidence, m.previous_value, m.new_value, m.recorded_at FROM metadata_provenance m "
        "JOIN publication p ON p.id = m.publication_id WHERE " PAPERTRAIL_IN_LIBRARY("p") " ORDER BY m.id",
        [](Statement &s) {
            return QJsonObject{{QStringLiteral("publication"), s.textAt(0)},
                               {QStringLiteral("field"), s.textAt(1)},
                               {QStringLiteral("source"), s.textAt(2)},
                               {QStringLiteral("confidence"), numberOrNull(s, 3)},
                               {QStringLiteral("previous"), textOrNull(s, 4)},
                               {QStringLiteral("new"), textOrNull(s, 5)},
                               {QStringLiteral("recordedAt"), double(s.int64At(6))}};
        });

    const QJsonArray summaryArray = rows(
        db,
        "SELECT s.uuid, p.uuid, s.model, s.effort, s.basis, s.identification_unsure, s.summary_text, s.sources_json, "
        "s.cost_dollars, s.sent_paper, s.created_at FROM publication_summary s JOIN publication p ON p.id = s.publication_id "
        "WHERE " PAPERTRAIL_IN_LIBRARY("p") " ORDER BY s.id",
        [](Statement &s) {
            return QJsonObject{{QStringLiteral("uuid"), s.textAt(0)},
                               {QStringLiteral("publication"), s.textAt(1)},
                               {QStringLiteral("model"), s.textAt(2)},
                               {QStringLiteral("effort"), s.textAt(3)},
                               {QStringLiteral("basis"), s.textAt(4)},
                               {QStringLiteral("identificationUnsure"), s.intAt(5) != 0},
                               {QStringLiteral("text"), s.textAt(6)},
                               {QStringLiteral("sources"), s.textAt(7)},
                               {QStringLiteral("costDollars"), numberOrNull(s, 8)},
                               {QStringLiteral("sentPaper"), s.intAt(9) != 0},
                               {QStringLiteral("createdAt"), double(s.int64At(10))}};
        });
    report.summaries = summaryArray.size();

    // Only those known by contents: a file that couldn't be read is only known on this computer.
    QJsonArray removedArray;
    {
        auto select = db.prepare("SELECT sha256, title, file_name, removed_at FROM removed_content WHERE sha256 IS NOT NULL ORDER BY id");
        while (select->step()) {
            removedArray.append(QJsonObject{{QStringLiteral("sha256"), select->textAt(0)},
                                            {QStringLiteral("title"), select->textAt(1)},
                                            {QStringLiteral("fileName"), select->textAt(2)},
                                            {QStringLiteral("removedAt"), double(select->int64At(3))}});
        }
    }
    report.removed = removedArray.size();

    const QJsonObject settings = core::SettingsStore(db).exportAll();

    const QJsonObject manifest{
        {QStringLiteral("format"), QStringLiteral("papertrail-library")},
        {QStringLiteral("version"), kBundleVersion},
        {QStringLiteral("exportedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("counts"),
         QJsonObject{{QStringLiteral("publications"), double(report.publications)},
                     {QStringLiteral("tags"), double(report.tags)},
                     {QStringLiteral("collections"), double(report.collections)},
                     {QStringLiteral("annotations"), double(report.annotations)},
                     {QStringLiteral("summaries"), double(report.summaries)}}}};

    const auto json = [](const QJsonValue &value) {
        return value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                               : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    };
    ZipWriter zip;
    zip.add(QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    zip.add(QStringLiteral("publications.json"), json(publicationArray));
    zip.add(QStringLiteral("tags.json"), json(tagArray));
    zip.add(QStringLiteral("collections.json"), json(collectionArray));
    zip.add(QStringLiteral("saved_searches.json"), json(savedSearchArray));
    zip.add(QStringLiteral("annotations.json"), json(annotationArray));
    zip.add(QStringLiteral("reading.json"), json(readingArray));
    zip.add(QStringLiteral("provenance.json"), json(provenanceArray));
    zip.add(QStringLiteral("summaries.json"), json(summaryArray));
    zip.add(QStringLiteral("removed.json"), json(removedArray));
    zip.add(QStringLiteral("settings.json"), json(settings));
    return zip.finish();
}

ExportReport exportLibrary(Database &db, const QString &path, const Progress &progress)
{
    ExportReport report;
    const QByteArray data = buildBundle(db, report, progress);
    if (report.cancelled)
        return report;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error(QStringLiteral("%1 couldn't be written: %2").arg(path, file.errorString()).toStdString());
    return report;
}

// ---------------------------------------------------------------------------------------------------- Reading

std::optional<Bundle> readBundle(const QByteArray &data, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return std::nullopt;
    };
    ZipReader zip;
    if (!zip.open(data))
        return fail(QStringLiteral("This isn't a Papertrail library export: %1.").arg(zip.error()));
    const std::optional<QByteArray> manifestData = zip.read(QStringLiteral("manifest.json"));
    if (!manifestData)
        return fail(QStringLiteral("This isn't a Papertrail library export: it has no manifest."));
    const QJsonObject manifest = QJsonDocument::fromJson(*manifestData).object();
    if (manifest.value(QStringLiteral("format")).toString() != QLatin1StringView("papertrail-library"))
        return fail(QStringLiteral("This isn't a Papertrail library export."));

    Bundle bundle;
    bundle.manifest = manifest;
    bundle.version = manifest.value(QStringLiteral("version")).toInt();
    if (bundle.version < 1)
        return fail(QStringLiteral("This export's version can't be read."));
    if (bundle.version > kBundleVersion)
        return fail(QStringLiteral("This export was made by a newer version of Papertrail. Update Papertrail to import it."));

    // Each part is optional, so that an export missing one, or a later version without it, can still be read.
    const auto array = [&](const QString &name, QJsonArray &into) -> bool {
        if (!zip.contains(name))
            return true;
        const std::optional<QByteArray> part = zip.read(name);
        if (!part)
            return false;
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(*part, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isArray())
            return false;
        into = document.array();
        return true;
    };
    if (!array(QStringLiteral("publications.json"), bundle.publications) || !array(QStringLiteral("tags.json"), bundle.tags)
        || !array(QStringLiteral("collections.json"), bundle.collections)
        || !array(QStringLiteral("saved_searches.json"), bundle.savedSearches)
        || !array(QStringLiteral("annotations.json"), bundle.annotations)
        || !array(QStringLiteral("reading.json"), bundle.readingStates)
        || !array(QStringLiteral("provenance.json"), bundle.provenance)
        || !array(QStringLiteral("summaries.json"), bundle.summaries)
        || !array(QStringLiteral("removed.json"), bundle.removed))
        return fail(QStringLiteral("This export is damaged: %1.").arg(zip.error().isEmpty() ? QStringLiteral("a part can't be read") : zip.error()));
    if (zip.contains(QStringLiteral("settings.json"))) {
        const std::optional<QByteArray> part = zip.read(QStringLiteral("settings.json"));
        if (!part)
            return fail(QStringLiteral("This export is damaged: %1.").arg(zip.error()));
        bundle.settings = QJsonDocument::fromJson(*part).object();
    }
    return bundle;
}

std::optional<Bundle> readBundleFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1 couldn't be opened: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    return readBundle(file.readAll(), error);
}

// ---------------------------------------------------------------------------------------------------- Import

namespace {

class Importer {
public:
    Importer(Database &db, const Bundle &bundle, const ImportOptions &options, ImportReport &report)
        : m_db(db), m_bundle(bundle), m_options(options), m_report(report), m_publications(db), m_files(db)
    {
        for (const core::LibraryRoot &root : m_files.removedRoots())
            m_removedRoots.insert(root.id);
    }

    bool run()
    {
        const qint64 total = m_bundle.publications.size();
        if (m_options.parts.tags)
            importTags();
        if (!importPublications(total))
            return false;
        if (m_options.parts.tags) {
            importCollections();
            importSavedSearches();
        }
        if (m_options.parts.annotations)
            importAnnotations();
        if (m_options.parts.readingPlaces)
            importReadingPlaces();
        if (m_options.parts.details) {
            importProvenance();
            importSummaries();
        }
        if (m_options.parts.settings)
            m_report.settings = core::SettingsStore(m_db).importAll(m_bundle.settings);
        return true;
    }

private:
    bool cancelled() const { return m_options.progress.cancelled && m_options.progress.cancelled(); }

    // A publication here for an export's uuid: one imported before, or found here since then.
    std::optional<qint64> localPublication(const QString &uuid)
    {
        if (const auto mapped = m_publicationIds.constFind(uuid); mapped != m_publicationIds.cend())
            return *mapped;
        return m_publications.idForUuid(uuid);
    }

    // ---- Tags
    void importTags()
    {
        auto byUuid = m_db.prepare("SELECT id FROM tag WHERE uuid = ?");
        auto byName = m_db.prepare("SELECT id FROM tag WHERE name = ? COLLATE NOCASE AND parent_id IS ?");
        auto insert = m_db.prepare("INSERT INTO tag (uuid, name, parent_id, colour, created_at) VALUES (?, ?, ?, ?, ?)");
        for (const QJsonValue &value : m_bundle.tags) {
            const QJsonObject tag = value.toObject();
            const QString uuid = tag.value(QStringLiteral("uuid")).toString();
            const QString name = tag.value(QStringLiteral("name")).toString().trimmed();
            if (uuid.isEmpty() || name.isEmpty())
                continue;
            byUuid->bind(1, uuid);
            if (byUuid->step()) {
                m_tagIds.insert(uuid, byUuid->int64At(0));
                byUuid->reset();
                continue;
            }
            byUuid->reset();
            std::optional<qint64> parent;
            const QString parentUuid = textValue(tag.value(QStringLiteral("parent")));
            if (!parentUuid.isEmpty())
                parent = m_tagIds.value(parentUuid, 0) ? std::optional<qint64>(m_tagIds.value(parentUuid)) : std::nullopt;
            // A tag with the same name in the same place is the same tag, made on each computer separately.
            byName->bind(1, name);
            byName->bind(2, parent);
            if (byName->step()) {
                m_tagIds.insert(uuid, byName->int64At(0));
                byName->reset();
                continue;
            }
            byName->reset();
            insert->bind(1, uuid);
            insert->bind(2, name);
            insert->bind(3, parent);
            insert->bind(4, textValue(tag.value(QStringLiteral("colour"))));
            insert->bind(5, int64Value(tag.value(QStringLiteral("createdAt"))));
            insert->step();
            insert->reset();
            m_tagIds.insert(uuid, m_db.lastInsertId());
            ++m_report.tagsAdded;
        }
    }

    // ---- Publications
    bool importPublications(qint64 total)
    {
        // A publication that's here already, with the same contents found separately, is the same one.
        for (qsizetype i = 0; i < m_bundle.publications.size(); ++i) {
            if (cancelled())
                return false;
            const QJsonObject object = m_bundle.publications.at(i).toObject();
            const QString uuid = object.value(QStringLiteral("uuid")).toString();
            if (uuid.isEmpty())
                continue;

            std::optional<qint64> local = m_publications.idForUuid(uuid);
            bool foundByContents = false;
            if (!local && !object.contains(QStringLiteral("parent"))) {
                local = publicationWithContents(object.value(QStringLiteral("files")).toArray());
                foundByContents = local.has_value();
            }

            if (m_options.parts.details) {
                if (local)
                    updatePublication(*local, object, foundByContents);
                else
                    local = addPublication(object);
            }
            if (!local)
                continue; // Not here, and details weren't asked for.
            m_publicationIds.insert(uuid, *local);
            m_imported.insert(*local);

            if (m_options.parts.tags) {
                Statement &link = m_db.cached("INSERT OR IGNORE INTO publication_tag (publication_id, tag_id) VALUES (?, ?)");
                for (const QJsonValue &tag : object.value(QStringLiteral("tags")).toArray()) {
                    const qint64 tagId = m_tagIds.value(tag.toString(), 0);
                    if (tagId == 0)
                        continue;
                    link.bind(1, *local);
                    link.bind(2, tagId);
                    link.step();
                    link.reset();
                }
            }
            if (m_options.parts.details)
                findFiles(*local, object);

            if (m_options.progress.progress && (i % 200 == 0 || i + 1 == m_bundle.publications.size()))
                m_options.progress.progress(i + 1, total);
        }
        return true;
    }

    std::optional<qint64> publicationWithContents(const QJsonArray &files)
    {
        for (const QJsonValue &value : files) {
            const QString sha = value.toObject().value(QStringLiteral("sha256")).toString().toLower();
            if (sha.isEmpty() || int64Value(value.toObject().value(QStringLiteral("size"))) == 0)
                continue;
            for (const FileRecord &file : m_files.withHash(sha)) {
                if (m_removedRoots.contains(file.rootId))
                    continue;
                for (const qint64 id : m_files.publicationsFor(file.id)) {
                    // Each publication here stands for one in the export, not two.
                    if (!m_matchedLocally.contains(id)) {
                        m_matchedLocally.insert(id);
                        return id;
                    }
                }
            }
        }
        return std::nullopt;
    }

    qint64 addPublication(const QJsonObject &object)
    {
        Publication p;
        publicationFromJson(p, object);
        p.uuid = object.value(QStringLiteral("uuid")).toString();
        p.createdAt = int64Value(object.value(QStringLiteral("createdAt")));
        p.updatedAt = int64Value(object.value(QStringLiteral("updatedAt")));
        const QString parent = textValue(object.value(QStringLiteral("parent")));
        if (!parent.isEmpty()) {
            if (const std::optional<qint64> parentId = localPublication(parent)) {
                p.parentId = parentId;
                p.pageStart = intFrom(object.value(QStringLiteral("pageStart")));
                p.pageEnd = intFrom(object.value(QStringLiteral("pageEnd")));
            }
        }
        if (p.title.trimmed().isEmpty())
            p.title = QStringLiteral("Untitled");
        const qint64 id = m_publications.create(p);
        ++m_report.added;
        return id;
    }

    // The more recently edited wins. One found by its contents was made here by scanning, so the export's
    // details, which someone may have corrected, are taken, and what's only here is kept.
    void updatePublication(qint64 id, const QJsonObject &object, bool foundByContents)
    {
        const std::optional<Publication> current = m_publications.get(id);
        if (!current)
            return;
        Publication incoming = *current;
        publicationFromJson(incoming, object);
        const qint64 incomingUpdated = int64Value(object.value(QStringLiteral("updatedAt")));

        Publication result = *current;
        bool takeIncoming = foundByContents || incomingUpdated > current->updatedAt;
        if (takeIncoming) {
            result = incoming;
            PublicationRepository::fillGaps(result, *current); // Merging only ever adds.
        } else {
            PublicationRepository::fillGaps(result, incoming);
        }
        if (foundByContents) {
            // It takes the export's identity, so the two libraries know it as the same publication from now on.
            const QString uuid = object.value(QStringLiteral("uuid")).toString();
            if (!m_publications.idForUuid(uuid)) {
                auto rename = m_db.prepare("UPDATE publication SET uuid = ? WHERE id = ?");
                rename->bind(1, uuid);
                rename->bind(2, id);
                rename->step();
            }
        }
        if (sameDetails(result, *current)) {
            ++m_report.unchanged;
            return;
        }
        m_publications.updateWithProvenance(result, QString::fromLatin1(kImportSource));
        if (takeIncoming && !foundByContents && incomingUpdated > 0) {
            // So that importing the same export again finds nothing newer.
            auto stamp = m_db.prepare("UPDATE publication SET updated_at = ? WHERE id = ?");
            stamp->bind(1, incomingUpdated);
            stamp->bind(2, id);
            stamp->step();
        }
        ++m_report.updated;
    }

    // Files are found by their contents first. Failing that, a possible match is suggested, and the
    // publication waits for a file with its contents to turn up.
    void findFiles(qint64 id, const QJsonObject &object)
    {
        const QJsonArray fileArray = object.value(QStringLiteral("files")).toArray();
        if (fileArray.isEmpty())
            return;
        bool linked = false;
        QList<core::FileRepository::ExpectedFile> missing;
        for (const QJsonValue &value : fileArray) {
            const QJsonObject file = value.toObject();
            core::FileRepository::ExpectedFile expected;
            expected.sha256 = file.value(QStringLiteral("sha256")).toString().toLower();
            expected.size = int64Value(file.value(QStringLiteral("size")));
            expected.format = textValue(file.value(QStringLiteral("format")));
            expected.fileName = textValue(file.value(QStringLiteral("name")));
            expected.primary = file.value(QStringLiteral("primary")).toBool();
            if (expected.sha256.isEmpty())
                continue;
            bool found = false;
            if (expected.size > 0) {
                for (const FileRecord &here : m_files.withHash(expected.sha256)) {
                    if (m_removedRoots.contains(here.rootId))
                        continue;
                    const QList<qint64> owners = m_files.publicationsFor(here.id);
                    if (owners.contains(id)) {
                        found = true;
                        break;
                    }
                    if (!owners.isEmpty())
                        continue; // Another publication's, which only a proposal can change.
                    m_files.link(id, here.id, expected.primary);
                    found = true;
                    break;
                }
            }
            if (found)
                linked = true;
            else
                missing << expected;
        }
        if (linked && missing.isEmpty()) {
            ++m_report.linked;
            return;
        }
        for (const auto &expected : std::as_const(missing))
            m_files.expectFile(id, expected);
        if (linked) {
            ++m_report.linked; // Another format may still turn up.
            return;
        }
        if (proposeFiles(id, missing))
            ++m_report.proposed;
        else
            ++m_report.waiting;
    }

    // Each lookup goes through an index, since this runs for every publication still waiting for a file, and
    // a search through the whole library each time would grow with the square of its size.
    bool proposeFiles(qint64 id, const QList<core::FileRepository::ExpectedFile> &missing)
    {
        const std::optional<Publication> p = m_publications.get(id);
        if (!p)
            return false;
        QSet<qint64> proposed;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const auto propose = [&](qint64 fileId, const char *reason) {
            if (proposed.contains(fileId))
                return;
            proposed.insert(fileId);
            Statement &insert = m_db.cached(
                "INSERT OR IGNORE INTO file_proposal (publication_id, file_id, reason, created_at) VALUES (?, ?, ?, ?)");
            insert.bind(1, id);
            insert.bind(2, fileId);
            insert.bind(3, reason);
            insert.bind(4, now);
            insert.step();
            insert.reset();
        };

        // Another publication here with the same identifier, such as another download of the same paper.
        const auto sameIdentifier = [&](Statement &others, const QString &value) {
            if (value.isEmpty())
                return;
            others.bind(1, value);
            others.bind(2, id);
            QList<qint64> files;
            while (others.step())
                files << others.int64At(0);
            others.reset();
            for (const qint64 file : std::as_const(files)) {
                // Not one of the publications this export brought in.
                const QList<qint64> owners = m_files.publicationsFor(file);
                if (std::any_of(owners.cbegin(), owners.cend(), [&](qint64 owner) { return m_imported.contains(owner); }))
                    continue;
                propose(file, "identifier");
            }
        };
        sameIdentifier(m_db.cached("SELECT pf.file_id FROM publication q JOIN publication_file pf ON pf.publication_id = q.id "
                                   "JOIN file f ON f.id = pf.file_id WHERE q.doi = ? AND q.id <> ? "
                                   "AND f.root_id NOT IN (SELECT root_id FROM removed_root) ORDER BY pf.is_primary DESC"),
                       p->doi.trimmed().toLower());
        sameIdentifier(m_db.cached("SELECT pf.file_id FROM publication q JOIN publication_file pf ON pf.publication_id = q.id "
                                   "JOIN file f ON f.id = pf.file_id WHERE q.isbn13 = ? AND q.id <> ? "
                                   "AND f.root_id NOT IN (SELECT root_id FROM removed_root) ORDER BY pf.is_primary DESC"),
                       p->isbn13.trimmed());
        sameIdentifier(m_db.cached("SELECT pf.file_id FROM publication q JOIN publication_file pf ON pf.publication_id = q.id "
                                   "JOIN file f ON f.id = pf.file_id WHERE q.arxiv_id = ? AND q.id <> ? "
                                   "AND f.root_id NOT IN (SELECT root_id FROM removed_root) ORDER BY pf.is_primary DESC"),
                       p->arxivId.trimmed());

        // A file with the same name and size, but other contents, such as one saved again.
        Statement &sameName = m_db.cached(
            "SELECT id FROM file WHERE file_name = ? AND size = ? AND root_id NOT IN (SELECT root_id FROM removed_root)");
        for (const auto &expected : missing) {
            if (expected.fileName.isEmpty() || expected.size <= 0)
                continue;
            sameName.bind(1, expected.fileName);
            sameName.bind(2, expected.size);
            QList<qint64> files;
            while (sameName.step())
                files << sameName.int64At(0);
            sameName.reset();
            for (const qint64 file : std::as_const(files)) {
                if (!m_files.publicationsFor(file).contains(id))
                    propose(file, "name_and_size");
            }
        }
        return !proposed.isEmpty();
    }

    // ---- Saved searches. One already here, by uuid, is left as it is.
    void importSavedSearches()
    {
        auto byUuid = m_db.prepare("SELECT 1 FROM saved_search WHERE uuid = ?");
        auto byName = m_db.prepare("SELECT 1 FROM saved_search WHERE name = ? COLLATE NOCASE");
        auto insert = m_db.prepare("INSERT INTO saved_search (uuid, name, definition, created_at) VALUES (?, ?, ?, ?)");
        const auto has = [](core::Statement &select, const QString &value) {
            select.bind(1, value);
            const bool found = select.step();
            select.reset();
            return found;
        };
        for (const QJsonValue &value : m_bundle.savedSearches) {
            const QJsonObject search = value.toObject();
            const QString uuid = search.value(QStringLiteral("uuid")).toString();
            QString name = search.value(QStringLiteral("name")).toString().trimmed();
            if (uuid.isEmpty() || name.isEmpty() || has(*byUuid, uuid))
                continue;
            // A different search with the same name keeps its name, and this one says where it came from.
            if (has(*byName, name)) {
                name = QStringLiteral("%1 (imported)").arg(name);
                if (has(*byName, name))
                    continue;
            }
            insert->bind(1, uuid);
            insert->bind(2, name);
            insert->bind(3, QString::fromUtf8(QJsonDocument(search.value(QStringLiteral("definition")).toObject()).toJson(QJsonDocument::Compact)));
            insert->bind(4, int64Value(search.value(QStringLiteral("createdAt"))));
            insert->step();
            insert->reset();
        }
    }

    // ---- Collections
    void importCollections()
    {
        auto byUuid = m_db.prepare("SELECT id FROM collection WHERE uuid = ?");
        auto byName = m_db.prepare("SELECT id FROM collection WHERE name = ?");
        auto insert = m_db.prepare("INSERT INTO collection (uuid, name, created_at, updated_at) VALUES (?, ?, ?, ?)");
        auto member = m_db.prepare(
            "INSERT OR IGNORE INTO collection_publication (collection_id, publication_id, added_at) VALUES (?, ?, ?)");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (const QJsonValue &value : m_bundle.collections) {
            const QJsonObject collection = value.toObject();
            const QString uuid = collection.value(QStringLiteral("uuid")).toString();
            const QString name = collection.value(QStringLiteral("name")).toString().trimmed();
            if (uuid.isEmpty() || name.isEmpty())
                continue;
            qint64 id = 0;
            byUuid->bind(1, uuid);
            if (byUuid->step())
                id = byUuid->int64At(0);
            byUuid->reset();
            if (id == 0) {
                byName->bind(1, name);
                if (byName->step())
                    id = byName->int64At(0);
                byName->reset();
            }
            if (id == 0) {
                insert->bind(1, uuid);
                insert->bind(2, name);
                insert->bind(3, int64Value(collection.value(QStringLiteral("createdAt"))));
                insert->bind(4, now);
                insert->step();
                insert->reset();
                id = m_db.lastInsertId();
                ++m_report.collectionsAdded;
            }
            for (const QJsonValue &publication : collection.value(QStringLiteral("publications")).toArray()) {
                const std::optional<qint64> local = localPublication(publication.toString());
                if (!local)
                    continue;
                member->bind(1, id);
                member->bind(2, *local);
                member->bind(3, now);
                member->step();
                member->reset();
            }
        }

        // Then which is inside which, once they all exist. One already placed here stays where it is.
        core::CollectionRepository collections(m_db);
        for (const QJsonValue &value : m_bundle.collections) {
            const QJsonObject collection = value.toObject();
            const QString parentUuid = collection.value(QStringLiteral("parentUuid")).toString();
            if (parentUuid.isEmpty())
                continue;
            const auto idFor = [&](const QString &uuid) -> std::optional<qint64> {
                auto select = m_db.prepare("SELECT id FROM collection WHERE uuid = ?");
                select->bind(1, uuid);
                return select->step() ? std::optional<qint64>(select->int64At(0)) : std::nullopt;
            };
            const std::optional<qint64> child = idFor(collection.value(QStringLiteral("uuid")).toString());
            const std::optional<qint64> parent = idFor(parentUuid);
            if (!child || !parent)
                continue;
            if (collections.get(*child)->parentId)
                continue;
            try {
                collections.move(*child, *parent);
            } catch (const std::invalid_argument &) {
                // It would go inside itself, from how this library has them. It stays at the top.
            }
        }
    }

    // ---- Highlights and notes
    void importAnnotations()
    {
        auto existing = m_db.prepare("SELECT id, updated_at FROM annotation WHERE uuid = ?");
        auto insert = m_db.prepare(
            "INSERT INTO annotation (uuid, publication_id, content_hash, location_json, type, colour, selected_text, note, "
            "needs_review, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        auto update = m_db.prepare(
            "UPDATE annotation SET content_hash = ?, location_json = ?, type = ?, colour = ?, selected_text = ?, note = ?, "
            "needs_review = ?, updated_at = ? WHERE id = ?");
        static const QSet<QString> types = {QStringLiteral("highlight"), QStringLiteral("underline"), QStringLiteral("note")};
        for (const QJsonValue &value : m_bundle.annotations) {
            const QJsonObject a = value.toObject();
            const QString uuid = a.value(QStringLiteral("uuid")).toString();
            const QString type = a.value(QStringLiteral("type")).toString();
            const std::optional<qint64> publication = localPublication(a.value(QStringLiteral("publication")).toString());
            if (uuid.isEmpty() || !publication || !types.contains(type))
                continue;
            const qint64 updatedAt = int64Value(a.value(QStringLiteral("updatedAt")));
            existing->bind(1, uuid);
            if (existing->step()) {
                const qint64 id = existing->int64At(0);
                const qint64 localUpdated = existing->int64At(1);
                existing->reset();
                if (updatedAt > localUpdated) {
                    update->bind(1, textValue(a.value(QStringLiteral("contentHash"))));
                    update->bind(2, a.value(QStringLiteral("location")).toString());
                    update->bind(3, type);
                    update->bind(4, textValue(a.value(QStringLiteral("colour"))));
                    update->bind(5, textValue(a.value(QStringLiteral("selectedText"))));
                    update->bind(6, textValue(a.value(QStringLiteral("note"))));
                    update->bind(7, a.value(QStringLiteral("needsReview")).toBool() ? 1 : 0);
                    update->bind(8, updatedAt);
                    update->bind(9, id);
                    update->step();
                    update->reset();
                    ++m_report.annotationsUpdated;
                }
                continue;
            }
            existing->reset();
            insert->bind(1, uuid);
            insert->bind(2, *publication);
            insert->bind(3, textValue(a.value(QStringLiteral("contentHash"))));
            insert->bind(4, a.value(QStringLiteral("location")).toString());
            insert->bind(5, type);
            insert->bind(6, textValue(a.value(QStringLiteral("colour"))));
            insert->bind(7, textValue(a.value(QStringLiteral("selectedText"))));
            insert->bind(8, textValue(a.value(QStringLiteral("note"))));
            insert->bind(9, a.value(QStringLiteral("needsReview")).toBool() ? 1 : 0);
            insert->bind(10, int64Value(a.value(QStringLiteral("createdAt"))));
            insert->bind(11, updatedAt);
            insert->step();
            insert->reset();
            ++m_report.annotationsAdded;
        }
    }

    // ---- Reading places
    void importReadingPlaces()
    {
        auto existing = m_db.prepare("SELECT last_opened FROM reading_state WHERE publication_id = ?");
        auto upsert = m_db.prepare(
            "INSERT INTO reading_state (publication_id, location, zoom, last_opened) VALUES (?, ?, ?, ?) "
            "ON CONFLICT (publication_id) DO UPDATE SET location = excluded.location, zoom = excluded.zoom, "
            "last_opened = excluded.last_opened");
        for (const QJsonValue &value : m_bundle.readingStates) {
            const QJsonObject r = value.toObject();
            const std::optional<qint64> publication = localPublication(r.value(QStringLiteral("publication")).toString());
            if (!publication)
                continue;
            const qint64 lastOpened = int64Value(r.value(QStringLiteral("lastOpened")));
            existing->bind(1, *publication);
            const bool here = existing->step();
            const qint64 localOpened = here ? existing->optInt64At(0).value_or(0) : 0;
            existing->reset();
            if (here && localOpened >= lastOpened)
                continue; // Read more recently here.
            upsert->bind(1, *publication);
            upsert->bind(2, textValue(r.value(QStringLiteral("location"))));
            if (r.value(QStringLiteral("zoom")).isDouble())
                upsert->bind(3, r.value(QStringLiteral("zoom")).toDouble());
            else
                upsert->bindNull(3);
            if (r.value(QStringLiteral("lastOpened")).isDouble())
                upsert->bind(4, lastOpened);
            else
                upsert->bindNull(4);
            upsert->step();
            upsert->reset();
            ++m_report.readingPlaces;
        }
    }

    // ---- History and summaries
    void importProvenance()
    {
        auto existing = m_db.prepare(
            "SELECT 1 FROM metadata_provenance WHERE publication_id = ? AND field = ? AND source = ? AND recorded_at = ?");
        auto insert = m_db.prepare(
            "INSERT INTO metadata_provenance (publication_id, field, source, confidence, previous_value, new_value, recorded_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)");
        for (const QJsonValue &value : m_bundle.provenance) {
            const QJsonObject m = value.toObject();
            const std::optional<qint64> publication = localPublication(m.value(QStringLiteral("publication")).toString());
            const QString field = m.value(QStringLiteral("field")).toString();
            const QString source = m.value(QStringLiteral("source")).toString();
            const qint64 recordedAt = int64Value(m.value(QStringLiteral("recordedAt")));
            if (!publication || field.isEmpty() || source.isEmpty())
                continue;
            existing->bind(1, *publication);
            existing->bind(2, field);
            existing->bind(3, source);
            existing->bind(4, recordedAt);
            const bool here = existing->step();
            existing->reset();
            if (here)
                continue;
            insert->bind(1, *publication);
            insert->bind(2, field);
            insert->bind(3, source);
            if (m.value(QStringLiteral("confidence")).isDouble())
                insert->bind(4, m.value(QStringLiteral("confidence")).toDouble());
            else
                insert->bindNull(4);
            insert->bind(5, textValue(m.value(QStringLiteral("previous"))));
            insert->bind(6, textValue(m.value(QStringLiteral("new"))));
            insert->bind(7, recordedAt);
            insert->step();
            insert->reset();
        }
    }

    void importSummaries()
    {
        auto existing = m_db.prepare("SELECT created_at FROM publication_summary WHERE publication_id = ?");
        auto uuidTaken = m_db.prepare("SELECT publication_id FROM publication_summary WHERE uuid = ?");
        auto remove = m_db.prepare("DELETE FROM publication_summary WHERE publication_id = ?");
        auto insert = m_db.prepare(
            "INSERT INTO publication_summary (uuid, publication_id, model, effort, basis, identification_unsure, summary_text, "
            "sources_json, cost_dollars, sent_paper, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        for (const QJsonValue &value : m_bundle.summaries) {
            const QJsonObject s = value.toObject();
            const QString uuid = s.value(QStringLiteral("uuid")).toString();
            const std::optional<qint64> publication = localPublication(s.value(QStringLiteral("publication")).toString());
            const QString text = s.value(QStringLiteral("text")).toString();
            if (uuid.isEmpty() || !publication || text.isEmpty())
                continue;
            const qint64 createdAt = int64Value(s.value(QStringLiteral("createdAt")));
            uuidTaken->bind(1, uuid);
            const bool already = uuidTaken->step();
            uuidTaken->reset();
            if (already)
                continue;
            existing->bind(1, *publication);
            if (existing->step()) {
                const qint64 localCreated = existing->int64At(0);
                existing->reset();
                if (localCreated >= createdAt)
                    continue; // One summary each, and the newer is kept.
                remove->bind(1, *publication);
                remove->step();
                remove->reset();
            } else {
                existing->reset();
            }
            insert->bind(1, uuid);
            insert->bind(2, *publication);
            insert->bind(3, s.value(QStringLiteral("model")).toString());
            insert->bind(4, s.value(QStringLiteral("effort")).toString());
            insert->bind(5, s.value(QStringLiteral("basis")).toString());
            insert->bind(6, s.value(QStringLiteral("identificationUnsure")).toBool() ? 1 : 0);
            insert->bind(7, text);
            insert->bind(8, s.value(QStringLiteral("sources")).toString(QStringLiteral("[]")));
            if (s.value(QStringLiteral("costDollars")).isDouble())
                insert->bind(9, s.value(QStringLiteral("costDollars")).toDouble());
            else
                insert->bindNull(9);
            insert->bind(10, s.value(QStringLiteral("sentPaper")).toBool() ? 1 : 0);
            insert->bind(11, createdAt);
            insert->step();
            insert->reset();
            ++m_report.summariesAdded;
        }
    }

    Database &m_db;
    const Bundle &m_bundle;
    const ImportOptions &m_options;
    ImportReport &m_report;
    PublicationRepository m_publications;
    FileRepository m_files;
    QSet<qint64> m_removedRoots;
    QHash<QString, qint64> m_tagIds;          // By uuid.
    QHash<QString, qint64> m_publicationIds;  // By uuid.
    QSet<qint64> m_matchedLocally;            // Publications here already standing for one in the export.
    QSet<qint64> m_imported;                  // Every publication here that this export brought in or matched.
};

// Everything a person put in the library goes, for Replace. Files and folders stay, since they describe
// what's on this computer, and the export's publications find their files among them again.
void clearLibrary(Database &db)
{
    db.exec("DELETE FROM file_proposal");
    db.exec("DELETE FROM expected_file");
    db.exec("DELETE FROM removed_content");
    db.exec("DELETE FROM lookup_candidate");
    db.exec("DELETE FROM job WHERE kind = 'lookup'");
    db.exec("DELETE FROM collection_publication");
    db.exec("DELETE FROM collection_parent");
    db.exec("DELETE FROM collection");
    db.exec("DELETE FROM saved_search");
    db.exec("DELETE FROM publication_summary");
    db.exec("DELETE FROM annotation");
    db.exec("DELETE FROM reading_state");
    db.exec("DELETE FROM metadata_provenance");
    db.exec("DELETE FROM publication_tag");
    db.exec("DELETE FROM tag");
    db.exec("DELETE FROM hidden_publication");
    db.exec("DELETE FROM publication_file");
    db.exec("DELETE FROM publication WHERE parent_id IS NOT NULL");
    db.exec("DELETE FROM publication");
    db.exec("DELETE FROM person WHERE id NOT IN (SELECT person_id FROM publication_person)");
}

} // namespace

ImportReport importBundle(Database &db, const Bundle &bundle, const ImportOptions &options)
{
    ImportReport report;
    if (options.mode == ImportMode::Replace) {
        if (options.backupPath.isEmpty())
            throw std::invalid_argument("replacing a library needs somewhere to keep a backup first");
        if (QFileInfo::exists(options.backupPath))
            throw std::invalid_argument("the backup's file already exists");
        // A complete copy, made by SQLite, before anything changes.
        auto backup = db.prepare("VACUUM INTO ?");
        backup->bind(1, options.backupPath);
        backup->step();
        report.backupPath = options.backupPath;
    }

    Transaction transaction(db);
    if (options.mode == ImportMode::Replace)
        clearLibrary(db);
    Importer importer(db, bundle, options, report);
    if (!importer.run()) {
        report = ImportReport{};
        report.cancelled = true;
        return report; // The transaction rolls back.
    }
    if (options.parts.details) {
        // Removed there, so kept out here too, unless it's in this library: merging never takes anything away.
        auto add = db.prepare(
            "INSERT INTO removed_content (sha256, title, file_name, removed_at) SELECT ?1, ?2, ?3, ?4 "
            "WHERE NOT EXISTS (SELECT 1 FROM removed_content WHERE sha256 = ?1) "
            "AND NOT EXISTS (SELECT 1 FROM file f JOIN publication_file pf ON pf.file_id = f.id WHERE f.sha256 = ?1) "
            "AND NOT EXISTS (SELECT 1 FROM expected_file WHERE sha256 = ?1)");
        for (const QJsonValue &value : bundle.removed) {
            const QJsonObject entry = value.toObject();
            const QString sha256 = entry.value(QStringLiteral("sha256")).toString().toLower();
            if (sha256.size() != 64)
                continue;
            add->reset();
            add->bind(1, sha256);
            add->bind(2, entry.value(QStringLiteral("title")).toString());
            add->bind(3, entry.value(QStringLiteral("fileName")).toString());
            add->bind(4, qint64(entry.value(QStringLiteral("removedAt")).toDouble()));
            add->step();
            report.removedAdded += db.changes();
        }
    }
    if (options.mode == ImportMode::Replace) {
        // Files the export doesn't account for are forgotten, so that the next scan finds them as new.
        db.exec("DELETE FROM file WHERE id NOT IN (SELECT file_id FROM publication_file)");
    }
    // What's waiting for its files stays out of sight until they're found.
    FileRepository(db).hideWaitingPublications();
    transaction.commit();
    return report;
}

// ---------------------------------------------------------------------------------------------------- Proposals

QList<FileProposal> fileProposals(Database &db)
{
    QList<FileProposal> proposals;
    auto select = db.prepare(
        "SELECT fp.id, fp.publication_id, p.title, fp.file_id, r.name, f.relative_path, fp.reason FROM file_proposal fp "
        "JOIN publication p ON p.id = fp.publication_id JOIN file f ON f.id = fp.file_id "
        "JOIN library_root r ON r.id = f.root_id ORDER BY p.title_sort, fp.id");
    while (select->step()) {
        FileProposal proposal;
        proposal.id = select->int64At(0);
        proposal.publicationId = select->int64At(1);
        proposal.publicationTitle = select->textAt(2);
        proposal.fileId = select->int64At(3);
        proposal.filePath = select->textAt(4) + u'/' + select->textAt(5);
        proposal.reason = select->textAt(6);
        proposals << proposal;
    }
    return proposals;
}

qint64 fileProposalCount(Database &db)
{
    auto count = db.prepare("SELECT count(*) FROM file_proposal");
    count->step();
    return count->int64At(0);
}

void acceptFileProposal(Database &db, qint64 proposalId)
{
    auto select = db.prepare("SELECT publication_id, file_id FROM file_proposal WHERE id = ?");
    select->bind(1, proposalId);
    if (!select->step())
        throw std::invalid_argument("that suggestion no longer exists");
    const qint64 publicationId = select->int64At(0);
    const qint64 fileId = select->int64At(1);
    select.reset();

    Transaction transaction(db);
    FileRepository files(db);
    const std::optional<FileRecord> file = files.get(fileId);
    if (!file)
        throw std::invalid_argument("that file is no longer in the library");

    // Highlights made on the other computer's copy may not line up with this one's pages.
    auto mark = db.prepare("UPDATE annotation SET needs_review = 1 WHERE publication_id = ? AND content_hash IS NOT ? "
                           "AND content_hash IS NOT NULL");
    mark->bind(1, publicationId);
    mark->bind(2, file->sha256);
    mark->step();

    const QList<qint64> owners = files.publicationsFor(fileId);
    for (const qint64 owner : owners) {
        if (owner != publicationId)
            core::mergePublications(db, publicationId, owner, QString::fromLatin1(kImportSource));
    }
    files.link(publicationId, fileId, files.filesFor(publicationId).isEmpty());

    auto clear = db.prepare("DELETE FROM file_proposal WHERE publication_id = ? OR file_id = ?");
    clear->bind(1, publicationId);
    clear->bind(2, fileId);
    clear->step();
    transaction.commit();
}

void rejectFileProposal(Database &db, qint64 proposalId)
{
    auto remove = db.prepare("DELETE FROM file_proposal WHERE id = ?");
    remove->bind(1, proposalId);
    remove->step();
}

} // namespace papertrail::transfer
