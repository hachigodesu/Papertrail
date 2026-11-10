#pragma once

#include "core/model/Publication.h"

#include <QJsonObject>
#include <QList>
#include <QRectF>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

enum class AnnotationType { Highlight, Underline, Note };

// One stretch of highlighted text on one page. There's more than one if a selection crosses pages.
struct AnnotationPart {
    QString text;

    // Where it is. A file with fixed pages has a page and rectangles, in points from the top left of the page.
    int page = -1; // Counting from 0.
    QList<QRectF> rects;

    // A reflowable file's pages move with its layout, so it has a place in the text to look near instead,
    // and which match of the words to take if they're repeated on that page.
    QString place;
    int occurrence = 0;

    // An area drawn over a page, such as a figure or an equation, rather than words chosen in it. It has one
    // rectangle, and `text` is whatever words are inside it, so that searching finds it. Only on fixed pages. It's
    // stored as a highlight marked as an area, so a version before areas shows it as a highlighted rectangle.
    bool area = false;
};

// Kept in Papertrail's database, and never in the document. It belongs to a file's contents, so it survives
// the file being moved or renamed, and stays with the publication if the file changes.
struct Annotation {
    qint64 id = 0;
    QString uuid;
    qint64 publicationId = 0;
    QString contentHash;
    AnnotationType type = AnnotationType::Highlight;
    QString colour; // As #rrggbb.
    QString note;
    QList<AnnotationPart> parts;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;

    QString selectedText() const;
    int firstPage() const; // -1 if only a place is known.
    bool isArea() const;
};

// What to list of the highlights and notes across the library.
enum class AnnotationKind { Any, Highlight, Underline, Note, Area };
struct AnnotationFilter {
    QString search;  // Only those whose words or note have all of these, each as the start of a word.
    QString colour;  // As #rrggbb, or empty for any.
    AnnotationKind kind = AnnotationKind::Any;
    QList<qint64> collectionIds; // With the collections inside them, and the articles of issues in them.
    QList<qint64> tagIds;        // With the tags beneath them.
    bool withNote = false;
    qint64 publicationId = 0;    // Only those made in this publication, or in its articles, or 0 for any.
    bool byPublication = false;  // By publication, then in document order, rather than most recently changed first.
    int limit = 2000;
};

// A highlight or note, with what it's in, for listing across the library.
struct AnnotationListing {
    Annotation annotation;
    QString publicationTitle;
};

class AnnotationRepository {
public:
    explicit AnnotationRepository(Database &db) : m_db(db) {}

    qint64 create(Annotation &annotation);
    std::optional<Annotation> get(qint64 id);
    void setNote(qint64 id, const QString &note);
    void setColour(qint64 id, const QString &colour);
    void setType(qint64 id, AnnotationType type);
    void remove(qint64 id);

    // Everything on one file's contents, whichever publication it was made under, such as an article of a volume.
    QList<Annotation> forContents(const QString &contentHash);

    // Everything made under this publication, in any format, and from any version of its file.
    QList<Annotation> forPublication(qint64 publicationId);
    qint64 countFor(qint64 publicationId);

    // Every highlight and note in the library, most recently changed first, leaving out those on publications
    // that are out of sight. With `search`, only those whose words or note have all its words, each as the start of
    // a word. `total` is set to how many there are in all, of which at most `limit` are returned.
    QList<AnnotationListing> everywhere(const QString &search, int limit, qint64 *total = nullptr);
    // The same, chosen by more than words.
    QList<AnnotationListing> everywhere(const AnnotationFilter &filter, qint64 *total = nullptr);

private:
    Database &m_db;
};

// The default colours offered, as #rrggbb.
const QStringList &annotationColours();

// A publication's highlights and notes as a Markdown document.
QString annotationsToMarkdown(const Publication &publication, const QList<Annotation> &annotations);

} // namespace papertrail::core
