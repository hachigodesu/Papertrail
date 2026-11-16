#pragma once

// Books downloaded as a zip of PDFs, one per part, as ScienceDirect gives them: which zips are such books, the order
// of their parts, and the order kept for stitching them into one PDF. Nothing here reads a file: the worker gathers
// what each part shows, and this decides.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace papertrail::import {

// A page number printed on a part's page, or its page label.
struct PrintedNumber {
    int page = 0;  // The page within the part, counting from 0.
    int value = 0; // The number printed there.
    bool roman = false;
};

// What the worker found in one PDF in the zip.
struct ZipPart {
    QString name; // The entry's name in the zip.
    qint64 size = 0;
    QString sha256;
    int pageCount = 0;
    QString problem; // Why it can't be stitched, if it can't: unreadable, or needs a password.
    QString title;   // Its own metadata title.
    QString heading; // The largest text on its first page.
    QList<PrintedNumber> numbers;
};

// A part in the order worked out, or chosen.
struct PlacedPart {
    ZipPart part;
    bool included = true;
    QString reason;      // Why it's where it is, as the dialog says it.
    bool placed = true;  // False if nothing said where it goes.
    bool hasStart = false;
    int start = 0;       // The printed number of its first page, if known.
    bool startRoman = false;
};

struct StitchPlan {
    QList<PlacedPart> parts;
    bool certain = false; // Every part placed by printed numbers that run on without gaps or overlaps.
    int unplaced = 0;
};

// The title and year ScienceDirect's names share, as "Chapter-3---Requirements_2011_Architecture-of-Network-Systems.pdf"
// does. Empty and 0 if the names don't share them.
struct BookFromNames {
    QString title;
    int year = 0;
};

// One part as kept in an order: enough to stitch it again without reading the zip's parts first.
struct OrderedPart {
    QString name;
    QString sha256;
    int pageCount = 0;
    bool included = true;
    QString title; // Its item's title in the stitched book's outline.
};

// Whether a zip's entries are a book's parts: at least two PDFs, and nothing else but folders and what computers
// leave behind (__MACOSX, .DS_Store, Thumbs.db). The PDFs' names, in the zip's order, if so.
bool isBookZip(const QStringList &entries, QStringList *pdfs = nullptr);

// A page number set alone at the top or bottom of a page, or at either end of a running head. `line` is one line of
// text from the page's top or bottom margin.
bool printedNumberIn(const QString &line, int &value, bool &roman);

// The order of a book's parts, from their printed page numbers where they have them, else from their names.
StitchPlan stitchOrder(const QList<ZipPart> &parts);

// A part's title for the outline and the dialog: "Chapter 3 – Requirements of network systems". Its first page's
// heading finishes a name that was cut short.
QString partTitle(const ZipPart &part);

BookFromNames bookFromNames(const QStringList &names);

// What the worker sends of each part and of a plan.
QJsonObject zipPartToJson(const ZipPart &part);
ZipPart zipPartFromJson(const QJsonObject &object);
QJsonArray planToJson(const StitchPlan &plan);
StitchPlan planFromJson(const QJsonArray &array);

// The order kept for a zip, as JSON.
QByteArray orderToJson(const QList<OrderedPart> &parts);
QList<OrderedPart> orderFromJson(const QByteArray &json);
QList<OrderedPart> orderedParts(const StitchPlan &plan);

// A short key for an order, which names the stitched PDF it makes, so stitching again never has to overwrite a file
// something may still have open.
QString orderKey(const QList<OrderedPart> &parts);

// Where the stitched PDF and its order are kept, beside `convertedPath`, the zip's converted copy's usual place.
QString orderPathFor(const QString &convertedPath);
QString stitchedPathFor(const QString &convertedPath, const QList<OrderedPart> &parts);
// Writes the order beside `convertedPath`, for the worker, unless it's there already. False if it can't be written.
bool writeOrderFile(const QString &convertedPath, const QList<OrderedPart> &parts);

// How a page of the stitched PDF of `from` maps to one of `to`'s: for each page of `from`, the page of `to` with the
// same part's same page (parts are known by their names in the zip, which differ even where two parts' contents are the same), or -1 if `to` leaves that part out.
QList<int> pageMapping(const QList<OrderedPart> &from, const QList<OrderedPart> &to);

// Which part, and which page within it, a page of the stitched PDF is. False if the page is past the end.
bool partOfPage(const QList<OrderedPart> &parts, int page, QString &partName, int &pageInPart);
// The stitched page of a part's page, or -1 if the part is left out.
int pageOfPart(const QList<OrderedPart> &parts, const QString &partName, int pageInPart);

} // namespace papertrail::import
