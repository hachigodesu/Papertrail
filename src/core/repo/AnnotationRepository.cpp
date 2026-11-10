#include "core/repo/AnnotationRepository.h"

#include "core/db/Database.h"
#include "core/model/PublicationFields.h"
#include "core/query/SearchParser.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <stdexcept>

namespace papertrail::core {

namespace {

const char *const kColumns =
    "SELECT id, uuid, publication_id, content_hash, location_json, type, colour, note, created_at, updated_at FROM annotation ";

QString typeToDb(AnnotationType type)
{
    switch (type) {
    case AnnotationType::Underline: return QStringLiteral("underline");
    case AnnotationType::Note: return QStringLiteral("note");
    default: return QStringLiteral("highlight");
    }
}

AnnotationType typeFromDb(const QString &text)
{
    if (text == QLatin1StringView("underline"))
        return AnnotationType::Underline;
    if (text == QLatin1StringView("note"))
        return AnnotationType::Note;
    return AnnotationType::Highlight;
}

QString locationToJson(const QList<AnnotationPart> &parts)
{
    QJsonArray array;
    for (const AnnotationPart &part : parts) {
        QJsonObject object;
        object.insert(QStringLiteral("text"), part.text);
        if (part.page >= 0)
            object.insert(QStringLiteral("page"), part.page);
        if (!part.rects.isEmpty()) {
            QJsonArray rects;
            for (const QRectF &rect : part.rects)
                rects.append(QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()});
            object.insert(QStringLiteral("rects"), rects);
        }
        if (!part.place.isEmpty()) {
            object.insert(QStringLiteral("place"), part.place);
            object.insert(QStringLiteral("occurrence"), part.occurrence);
        }
        if (part.area)
            object.insert(QStringLiteral("area"), true);
        array.append(object);
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("parts"), array}}).toJson(QJsonDocument::Compact));
}

QList<AnnotationPart> locationFromJson(const QString &json)
{
    QList<AnnotationPart> parts;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("parts")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        AnnotationPart part;
        part.text = object.value(QStringLiteral("text")).toString();
        part.page = object.value(QStringLiteral("page")).toInt(-1);
        part.place = object.value(QStringLiteral("place")).toString();
        part.occurrence = object.value(QStringLiteral("occurrence")).toInt();
        part.area = object.value(QStringLiteral("area")).toBool();
        const QJsonArray rects = object.value(QStringLiteral("rects")).toArray();
        for (const QJsonValue &rect : rects) {
            const QJsonArray r = rect.toArray();
            part.rects << QRectF(r.at(0).toDouble(), r.at(1).toDouble(), r.at(2).toDouble(), r.at(3).toDouble());
        }
        parts << part;
    }
    return parts;
}

Annotation annotationFrom(Statement &select)
{
    Annotation annotation;
    annotation.id = select.int64At(0);
    annotation.uuid = select.textAt(1);
    annotation.publicationId = select.int64At(2);
    annotation.contentHash = select.textAt(3);
    annotation.parts = locationFromJson(select.textAt(4));
    annotation.type = typeFromDb(select.textAt(5));
    annotation.colour = select.textAt(6);
    annotation.note = select.textAt(7);
    annotation.createdAt = select.int64At(8);
    annotation.updatedAt = select.int64At(9);
    return annotation;
}

// In document order, as far as that's known. Places in reflowable text sort by chapter and then position.
bool comesBefore(const Annotation &a, const Annotation &b)
{
    const int pageA = a.firstPage();
    const int pageB = b.firstPage();
    if (pageA != pageB)
        return pageA < pageB;
    const QString placeA = a.parts.isEmpty() ? QString() : a.parts.first().place;
    const QString placeB = b.parts.isEmpty() ? QString() : b.parts.first().place;
    if (placeA != placeB) {
        const double chapterA = placeA.section(u':', 0, 0).toDouble();
        const double chapterB = placeB.section(u':', 0, 0).toDouble();
        if (chapterA != chapterB)
            return chapterA < chapterB;
        return placeA.section(u':', 1, 1).toDouble() < placeB.section(u':', 1, 1).toDouble();
    }
    return a.createdAt < b.createdAt;
}

QList<Annotation> sorted(QList<Annotation> annotations)
{
    std::stable_sort(annotations.begin(), annotations.end(), comesBefore);
    return annotations;
}

void quoteLines(QString &out, const QString &text)
{
    const QStringList lines = text.split(u'\n');
    for (const QString &line : lines)
        out += QStringLiteral("> ") + line.trimmed() + u'\n';
}

} // namespace

QString Annotation::selectedText() const
{
    QStringList texts;
    for (const AnnotationPart &part : parts)
        texts << part.text;
    return texts.join(u'\n');
}

int Annotation::firstPage() const
{
    return parts.isEmpty() ? -1 : parts.first().page;
}

bool Annotation::isArea() const
{
    return !parts.isEmpty() && parts.first().area;
}

qint64 AnnotationRepository::create(Annotation &annotation)
{
    if (annotation.uuid.isEmpty())
        annotation.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (annotation.createdAt == 0)
        annotation.createdAt = now;
    annotation.updatedAt = now;
    auto insert = m_db.prepare(
        "INSERT INTO annotation (uuid, publication_id, content_hash, location_json, type, colour, selected_text, note, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert->bind(1, annotation.uuid);
    insert->bind(2, annotation.publicationId);
    insert->bind(3, annotation.contentHash);
    insert->bind(4, locationToJson(annotation.parts));
    insert->bind(5, typeToDb(annotation.type));
    insert->bind(6, annotation.colour);
    insert->bind(7, annotation.selectedText());
    insert->bind(8, annotation.note);
    insert->bind(9, annotation.createdAt);
    insert->bind(10, annotation.updatedAt);
    insert->step();
    annotation.id = m_db.lastInsertId();
    return annotation.id;
}

std::optional<Annotation> AnnotationRepository::get(qint64 id)
{
    auto select = m_db.prepare(QByteArray(kColumns) + "WHERE id = ?");
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return annotationFrom(*select);
}

void AnnotationRepository::setNote(qint64 id, const QString &note)
{
    auto update = m_db.prepare("UPDATE annotation SET note = ?, updated_at = ? WHERE id = ?");
    update->bind(1, note);
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, id);
    update->step();
}

void AnnotationRepository::setColour(qint64 id, const QString &colour)
{
    auto update = m_db.prepare("UPDATE annotation SET colour = ?, updated_at = ? WHERE id = ?");
    update->bind(1, colour);
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, id);
    update->step();
}

void AnnotationRepository::setType(qint64 id, AnnotationType type)
{
    auto update = m_db.prepare("UPDATE annotation SET type = ?, updated_at = ? WHERE id = ?");
    update->bind(1, typeToDb(type));
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, id);
    update->step();
}

void AnnotationRepository::remove(qint64 id)
{
    auto remove = m_db.prepare("DELETE FROM annotation WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

QList<Annotation> AnnotationRepository::forContents(const QString &contentHash)
{
    QList<Annotation> annotations;
    if (contentHash.isEmpty())
        return annotations;
    auto select = m_db.prepare(QByteArray(kColumns) + "WHERE content_hash = ?");
    select->bind(1, contentHash);
    while (select->step())
        annotations << annotationFrom(*select);
    return sorted(annotations);
}

QList<Annotation> AnnotationRepository::forPublication(qint64 publicationId)
{
    QList<Annotation> annotations;
    auto select = m_db.prepare(QByteArray(kColumns) + "WHERE publication_id = ?");
    select->bind(1, publicationId);
    while (select->step())
        annotations << annotationFrom(*select);
    return sorted(annotations);
}

QList<AnnotationListing> AnnotationRepository::everywhere(const QString &search, int limit, qint64 *total)
{
    AnnotationFilter filter;
    filter.search = search;
    filter.limit = limit;
    return everywhere(filter, total);
}

QList<AnnotationListing> AnnotationRepository::everywhere(const AnnotationFilter &filter, qint64 *total)
{
    const QString match = parseSearch(filter.search).contentsMatch;
    if (total)
        *total = 0;
    if (!filter.search.trimmed().isEmpty() && match.isEmpty())
        return {}; // Nothing searchable in it, such as only punctuation.

    // Ids go in as numbers, and everything else as values bound in order.
    const auto idList = [](const QList<qint64> &ids) {
        QStringList numbers;
        for (const qint64 id : ids)
            numbers << QString::number(id);
        return numbers.join(u',').toLatin1();
    };
    QByteArray where = "WHERE publication_id NOT IN (SELECT publication_id FROM hidden_publication) ";
    QStringList values;
    if (!match.isEmpty()) {
        where += "AND id IN (SELECT rowid FROM annotation_fts WHERE annotation_fts MATCH ?) ";
        values << match;
    }
    if (!filter.colour.isEmpty()) {
        where += "AND lower(colour) = lower(?) ";
        values << filter.colour;
    }
    switch (filter.kind) {
    case AnnotationKind::Any: break;
    case AnnotationKind::Highlight: where += "AND type = 'highlight' AND location_json NOT LIKE '%\"area\":true%' "; break;
    case AnnotationKind::Underline: where += "AND type = 'underline' "; break;
    case AnnotationKind::Note: where += "AND type = 'note' "; break;
    case AnnotationKind::Area: where += "AND location_json LIKE '%\"area\":true%' "; break;
    }
    if (filter.withNote)
        where += "AND note IS NOT NULL AND trim(note) <> '' ";
    if (filter.publicationId != 0) {
        const QByteArray id = QByteArray::number(filter.publicationId);
        where += "AND (publication_id = " + id + " OR publication_id IN (SELECT id FROM publication WHERE parent_id = " + id + ")) ";
    }
    // A publication in a collection or with a tag, or an article inside one that is.
    const auto ownedBy = [](const QByteArray &members) {
        return "AND (publication_id IN (" + members + ") OR publication_id IN (SELECT id FROM publication WHERE parent_id IN ("
               + members + "))) ";
    };
    if (!filter.collectionIds.isEmpty()) {
        where += ownedBy("SELECT publication_id FROM collection_publication WHERE collection_id IN (WITH RECURSIVE down(id) AS "
                         "(SELECT id FROM collection WHERE id IN (" + idList(filter.collectionIds) + ") UNION SELECT cp.collection_id "
                         "FROM collection_parent cp JOIN down ON cp.parent_id = down.id) SELECT id FROM down)");
    }
    if (!filter.tagIds.isEmpty()) {
        where += ownedBy("SELECT publication_id FROM publication_tag WHERE tag_id IN (WITH RECURSIVE down(id) AS "
                         "(SELECT id FROM tag WHERE id IN (" + idList(filter.tagIds) + ") UNION SELECT t.id FROM tag t "
                         "JOIN down ON t.parent_id = down.id) SELECT id FROM down)");
    }
    const auto bindAll = [&](Statement &statement) {
        for (qsizetype i = 0; i < values.size(); ++i)
            statement.bind(int(i + 1), values.at(i));
    };
    if (total) {
        auto count = m_db.prepare("SELECT count(*) FROM annotation " + where);
        bindAll(*count);
        count->step();
        *total = count->int64At(0);
    }
    const QByteArray order = filter.byPublication
                                 ? "ORDER BY (SELECT title_sort FROM publication WHERE id = annotation.publication_id), publication_id, "
                                   "created_at, id "
                                 : "ORDER BY updated_at DESC, id DESC ";
    auto select = m_db.prepare(QByteArray(kColumns) + where + order + "LIMIT " + QByteArray::number(qMax(0, filter.limit)));
    bindAll(*select);
    Statement &title = m_db.cached("SELECT title FROM publication WHERE id = ?");
    QList<AnnotationListing> found;
    while (select->step()) {
        AnnotationListing listing;
        listing.annotation = annotationFrom(*select);
        title.bind(1, listing.annotation.publicationId);
        if (title.step())
            listing.publicationTitle = title.textAt(0);
        title.reset();
        found << listing;
    }
    // Each publication's in the order they come in it.
    if (filter.byPublication) {
        for (qsizetype start = 0; start < found.size();) {
            qsizetype end = start;
            while (end < found.size() && found.at(end).annotation.publicationId == found.at(start).annotation.publicationId)
                ++end;
            std::stable_sort(found.begin() + start, found.begin() + end,
                             [](const AnnotationListing &a, const AnnotationListing &b) { return comesBefore(a.annotation, b.annotation); });
            start = end;
        }
    }
    return found;
}

qint64 AnnotationRepository::countFor(qint64 publicationId)
{
    auto select = m_db.prepare("SELECT count(*) FROM annotation WHERE publication_id = ?");
    select->bind(1, publicationId);
    select->step();
    return select->int64At(0);
}

const QStringList &annotationColours()
{
    static const QStringList colours = {QStringLiteral("#ffe45c"), QStringLiteral("#8fe388"), QStringLiteral("#7fc8f8"), QStringLiteral("#f7a1c4")};
    return colours;
}

QString annotationsToMarkdown(const Publication &publication, const QList<Annotation> &annotations)
{
    QString out = QStringLiteral("# ") + publication.title.simplified() + QStringLiteral("\n\n");
    if (!publication.authors.isEmpty())
        out += peopleToText(publication.authors).simplified() + QStringLiteral("\n\n");
    QStringList source;
    if (!publication.containerTitle.isEmpty())
        source << publication.containerTitle.simplified();
    if (publication.year)
        source << QString::number(*publication.year);
    if (!publication.doi.isEmpty())
        source << QStringLiteral("doi:") + publication.doi;
    if (!source.isEmpty())
        out += source.join(QStringLiteral(", ")) + QStringLiteral("\n\n");

    for (const Annotation &annotation : annotations) {
        const int page = annotation.firstPage();
        // The words inside an area, such as an equation's, are rarely worth reading on their own.
        if (annotation.isArea())
            out += page >= 0 ? QStringLiteral("## Area on page %1\n\n").arg(page + 1) : QStringLiteral("## Area\n\n");
        else
            out += page >= 0 ? QStringLiteral("## Page %1\n\n").arg(page + 1) : QStringLiteral("## Highlight\n\n");
        const QString text = annotation.isArea() ? QString() : annotation.selectedText().trimmed();
        if (!text.isEmpty()) {
            quoteLines(out, text);
            out += u'\n';
        }
        const QString note = annotation.note.trimmed();
        if (!note.isEmpty())
            out += note + QStringLiteral("\n\n");
    }
    return out;
}

} // namespace papertrail::core
