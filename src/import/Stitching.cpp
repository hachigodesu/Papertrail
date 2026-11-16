#include "import/Stitching.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

namespace papertrail::import {

namespace {

// Where a part belongs by its name alone.
enum class Zone { Front = 0, Body = 1, Back = 2, Unknown = 3 };

struct NameKind {
    Zone zone = Zone::Unknown;
    double rank = 0;
    QString what; // "front matter", "chapter 3", and so on, for the reason given.
};

QString baseName(const QString &entry)
{
    QString name = entry.section(u'/', -1);
    if (name.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        name.chop(4);
    return name;
}

// The part's own words from ScienceDirect's name, before "_2011_" and the book's title.
QString ownPartOfName(const QString &entry)
{
    static const QRegularExpression book(QStringLiteral("_(1[5-9]|20)\\d\\d_.*$"));
    QString name = baseName(entry);
    name.remove(book);
    return name;
}

QString wordsOf(const QString &text)
{
    QString words = text;
    words.replace(u'_', u' ').replace(u'-', u' ');
    return words.simplified();
}

std::optional<int> romanValue(const QString &text)
{
    static const QRegularExpression valid(
        QStringLiteral("^m{0,3}(cm|cd|d?c{0,3})(xc|xl|l?x{0,3})(ix|iv|v?i{0,3})$"), QRegularExpression::CaseInsensitiveOption);
    if (text.isEmpty() || !valid.match(text).hasMatch())
        return std::nullopt;
    static const QHash<QChar, int> values = {{u'i', 1}, {u'v', 5}, {u'x', 10}, {u'l', 50}, {u'c', 100}, {u'd', 500}, {u'm', 1000}};
    const QString lower = text.toLower();
    int total = 0;
    for (qsizetype i = 0; i < lower.size(); ++i) {
        const int value = values.value(lower.at(i));
        const int next = i + 1 < lower.size() ? values.value(lower.at(i + 1)) : 0;
        total += value < next ? -value : value;
    }
    return total;
}

QString romanText(int value)
{
    static const QList<QPair<int, QString>> numerals = {
        {1000, QStringLiteral("m")}, {900, QStringLiteral("cm")}, {500, QStringLiteral("d")}, {400, QStringLiteral("cd")},
        {100, QStringLiteral("c")},  {90, QStringLiteral("xc")},  {50, QStringLiteral("l")},  {40, QStringLiteral("xl")},
        {10, QStringLiteral("x")},   {9, QStringLiteral("ix")},   {5, QStringLiteral("v")},   {4, QStringLiteral("iv")},
        {1, QStringLiteral("i")}};
    QString text;
    for (const auto &[amount, numeral] : numerals) {
        while (value >= amount) {
            text += numeral;
            value -= amount;
        }
    }
    return text;
}

QString printed(int value, bool roman)
{
    return roman ? romanText(value) : QString::number(value);
}

NameKind kindOf(const QString &entry)
{
    const QString words = wordsOf(ownPartOfName(entry)).toLower();
    NameKind kind;

    static const QRegularExpression chapter(QStringLiteral("^(?:chapter|ch|section|lecture|unit)\\.?\\s*(\\d{1,3})\\b"));
    static const QRegularExpression numbered(QStringLiteral("^(\\d{1,3})\\b(?!\\s*(?:st|nd|rd|th)\\b)"));
    static const QRegularExpression appendix(QStringLiteral("^appendi(?:x|ces)\\b\\s*([a-z]\\b|\\d{1,2}\\b)?"));
    if (const auto match = chapter.match(words); match.hasMatch()) {
        kind.zone = Zone::Body;
        kind.rank = match.captured(1).toInt();
        kind.what = QStringLiteral("chapter %1").arg(match.captured(1).toInt());
        return kind;
    }
    if (const auto match = appendix.match(words); match.hasMatch()) {
        kind.zone = Zone::Back;
        const QString which = match.captured(1);
        kind.rank = which.isEmpty() ? 0 : which.at(0).isDigit() ? which.toInt() : which.at(0).unicode() - u'a' + 1;
        kind.what = QStringLiteral("an appendix");
        return kind;
    }
    if (const auto match = numbered.match(words); match.hasMatch()) {
        kind.zone = Zone::Body;
        kind.rank = match.captured(1).toInt();
        kind.what = QStringLiteral("part numbered %1").arg(match.captured(1).toInt());
        return kind;
    }

    struct Keyword {
        const char *words;
        Zone zone;
        double rank;
        const char *what;
    };
    static const Keyword keywords[] = {
        {"front matter", Zone::Front, 0, "front matter"},
        {"half title", Zone::Front, 1, "front matter"},
        {"series", Zone::Front, 2, "front matter"},
        {"title page", Zone::Front, 3, "front matter"},
        {"copyright", Zone::Front, 4, "front matter"},
        {"dedication", Zone::Front, 5, "front matter"},
        {"table of contents", Zone::Front, 6, "front matter"},
        {"contents", Zone::Front, 6, "front matter"},
        {"list of figures", Zone::Front, 7, "front matter"},
        {"list of tables", Zone::Front, 7, "front matter"},
        {"foreword", Zone::Front, 8, "front matter"},
        {"preface", Zone::Front, 9, "front matter"},
        {"acknowledg", Zone::Front, 10, "front matter"},
        {"about the author", Zone::Front, 11, "front matter"},
        {"about the editor", Zone::Front, 11, "front matter"},
        {"contributors", Zone::Front, 11, "front matter"},
        {"abbreviations", Zone::Front, 12, "front matter"},
        {"nomenclature", Zone::Front, 12, "front matter"},
        {"notation", Zone::Front, 12, "front matter"},
        {"introduction", Zone::Front, 13, "an introduction"},
        {"afterword", Zone::Back, -2, "back matter"},
        {"epilogue", Zone::Back, -2, "back matter"},
        {"glossary", Zone::Back, 50, "back matter"},
        {"bibliography", Zone::Back, 60, "back matter"},
        {"references", Zone::Back, 60, "back matter"},
        {"further reading", Zone::Back, 60, "back matter"},
        {"author index", Zone::Back, 69, "an index"},
        {"subject index", Zone::Back, 70, "an index"},
        {"index", Zone::Back, 70, "an index"},
    };
    for (const Keyword &keyword : keywords) {
        if (words.startsWith(QLatin1StringView(keyword.words))) {
            kind.zone = keyword.zone;
            kind.rank = keyword.rank;
            kind.what = QString::fromLatin1(keyword.what);
            return kind;
        }
    }
    return kind;
}

// The printed number of a part's first page, from the numbers found on its pages, which must agree.
struct Start {
    bool found = false;
    int value = 0;
    bool roman = false;
};

Start startOf(const ZipPart &part)
{
    // Each number says where the part starts. A page's number can be misread, so the most agreed on wins.
    QMap<QPair<bool, int>, int> votes;
    for (const PrintedNumber &number : part.numbers) {
        const int start = number.value - number.page;
        if (start >= 1)
            ++votes[qMakePair(number.roman, start)];
    }
    Start best;
    int bestVotes = 0;
    for (auto it = votes.constBegin(); it != votes.constEnd(); ++it) {
        if (it.value() > bestVotes) {
            bestVotes = it.value();
            best.found = true;
            best.roman = it.key().first;
            best.value = it.key().second;
        }
    }
    return best;
}

int compareNames(const QString &a, const QString &b)
{
    // Numbers in names compare as numbers, so "Chapter-10" comes after "Chapter-9".
    static const QRegularExpression number(QStringLiteral("\\d+"));
    const QString keyA = wordsOf(ownPartOfName(a)).toLower();
    const QString keyB = wordsOf(ownPartOfName(b)).toLower();
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < keyA.size() && j < keyB.size()) {
        if (keyA.at(i).isDigit() && keyB.at(j).isDigit()) {
            const auto matchA = number.match(keyA, i);
            const auto matchB = number.match(keyB, j);
            const qlonglong valueA = matchA.captured().left(9).toLongLong();
            const qlonglong valueB = matchB.captured().left(9).toLongLong();
            if (valueA != valueB)
                return valueA < valueB ? -1 : 1;
            i += matchA.capturedLength();
            j += matchB.capturedLength();
            continue;
        }
        if (keyA.at(i) != keyB.at(j))
            return keyA.at(i) < keyB.at(j) ? -1 : 1;
        ++i;
        ++j;
    }
    if (keyA.size() - i != keyB.size() - j)
        return keyA.size() - i < keyB.size() - j ? -1 : 1;
    return a.compare(b);
}

} // namespace

bool isBookZip(const QStringList &entries, QStringList *pdfs)
{
    QStringList found;
    for (const QString &entry : entries) {
        if (entry.endsWith(u'/'))
            continue; // A folder.
        const QString name = entry.section(u'/', -1);
        if (entry.startsWith(QStringLiteral("__MACOSX/")) || name.startsWith(QStringLiteral("._"))
            || name.compare(QStringLiteral(".DS_Store"), Qt::CaseInsensitive) == 0
            || name.compare(QStringLiteral("Thumbs.db"), Qt::CaseInsensitive) == 0
            || name.compare(QStringLiteral("desktop.ini"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        if (!name.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
            return false;
        found << entry;
    }
    if (found.size() < 2)
        return false;
    if (pdfs)
        *pdfs = found;
    return true;
}

bool printedNumberIn(const QString &line, int &value, bool &roman)
{
    const QString text = line.simplified();
    if (text.isEmpty() || text.size() > 160)
        return false;
    static const QRegularExpression alone(QStringLiteral("^(\\d{1,4}|[ivxlcdmIVXLCDM]{1,8})$"));
    if (const auto match = alone.match(text); match.hasMatch()) {
        const QString number = match.captured(1);
        if (number.at(0).isDigit()) {
            value = number.toInt();
            roman = false;
            return value > 0;
        }
        const std::optional<int> fromRoman = romanValue(number);
        if (!fromRoman)
            return false;
        value = *fromRoman;
        roman = true;
        return true;
    }

    // A running head with the page's number at one end: "56 Chapter 3 Requirements" or "Requirements 57". A footer
    // with a year or a link at its end isn't one.
    static const QRegularExpression notAHead(
        QStringLiteral("©|\\(c\\)|copyright|doi|https?:|www\\.|isbn|issn|all rights|elsevier|published|downloaded"),
        QRegularExpression::CaseInsensitiveOption);
    if (notAHead.match(text).hasMatch())
        return false;
    // A running head is a few words, where a line of running text in the margin ending in a number is a sentence.
    constexpr qsizetype kMostWordsInAHead = 10;
    if (text.count(u' ') > kMostWordsInAHead)
        return false;
    static const QRegularExpression leading(QStringLiteral("^(\\d{1,4})\\s+(\\p{L}.*)$"));
    static const QRegularExpression trailing(QStringLiteral("^(.*\\p{L}.*?)\\s+(\\d{1,4})$"));
    // "Chapter 3" or "Table 2" ends with a number that isn't the page's.
    static const QRegularExpression labelled(
        QStringLiteral("(?:chapter|part|section|volume|vol\\.?|no\\.?|fig\\.?|figure|table|appendix|unit|lecture|eq\\.?)$"),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = leading.match(text); match.hasMatch()) {
        value = match.captured(1).toInt();
        roman = false;
        return value > 0;
    }
    if (const auto match = trailing.match(text); match.hasMatch()) {
        if (labelled.match(match.captured(1).trimmed()).hasMatch())
            return false;
        value = match.captured(2).toInt();
        roman = false;
        return value > 0;
    }
    return false;
}

StitchPlan stitchOrder(const QList<ZipPart> &parts)
{
    struct Item {
        PlacedPart placed;
        NameKind kind;
        Start start;
    };
    QList<Item> items;
    for (const ZipPart &part : parts) {
        Item item;
        item.placed.part = part;
        item.placed.included = part.problem.isEmpty();
        item.kind = kindOf(part.name);
        item.start = part.problem.isEmpty() ? startOf(part) : Start{};
        item.placed.hasStart = item.start.found;
        item.placed.start = item.start.value;
        item.placed.startRoman = item.start.roman;
        items << item;
    }

    // The order the names alone give.
    QList<int> byName(items.size());
    for (int i = 0; i < items.size(); ++i)
        byName[i] = i;
    std::stable_sort(byName.begin(), byName.end(), [&](int a, int b) {
        const NameKind &kindA = items.at(a).kind;
        const NameKind &kindB = items.at(b).kind;
        if (kindA.zone != kindB.zone)
            return int(kindA.zone) < int(kindB.zone);
        if (kindA.rank != kindB.rank)
            return kindA.rank < kindB.rank;
        return compareNames(items.at(a).placed.part.name, items.at(b).placed.part.name) < 0;
    });

    // Parts with printed numbers go by them. Roman numbers come first, as front matter's do.
    QList<int> order;
    for (int index : std::as_const(byName)) {
        if (items.at(index).start.found)
            order << index;
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Start &startA = items.at(a).start;
        const Start &startB = items.at(b).start;
        if (startA.roman != startB.roman)
            return startA.roman;
        return startA.value < startB.value;
    });

    // The rest go after whatever comes before them by name, or first if nothing does. A part whose name says
    // nothing goes last.
    QList<int> unplaced;
    for (qsizetype n = 0; n < byName.size(); ++n) {
        const int index = byName.at(n);
        if (items.at(index).start.found)
            continue;
        if (items.at(index).kind.zone == Zone::Unknown) {
            unplaced << index;
            continue;
        }
        qsizetype at = 0;
        for (qsizetype back = n - 1; back >= 0; --back) {
            const qsizetype found = order.indexOf(byName.at(back));
            if (found >= 0) {
                at = found + 1;
                break;
            }
        }
        order.insert(at, index);
    }
    order << unplaced;

    StitchPlan plan;
    for (int index : std::as_const(order)) {
        Item &item = items[index];
        PlacedPart placed = item.placed;
        if (!placed.part.problem.isEmpty()) {
            placed.reason = placed.part.problem;
        } else if (item.start.found) {
            const int last = item.start.value + qMax(1, placed.part.pageCount) - 1;
            placed.reason = last == item.start.value
                ? QStringLiteral("Printed page %1").arg(printed(last, item.start.roman))
                : QStringLiteral("Printed pages %1–%2").arg(printed(item.start.value, item.start.roman), printed(last, item.start.roman));
        } else if (item.kind.zone != Zone::Unknown) {
            placed.reason = QStringLiteral("By its name: %1").arg(item.kind.what);
        } else {
            placed.placed = false;
            placed.reason = QStringLiteral("Couldn't tell where this goes");
            if (placed.included)
                ++plan.unplaced;
        }
        plan.parts << placed;
    }

    // Certain only if every part is placed by printed numbers that run on: the next starts where the last ended, or a
    // page later, where a blank page was left out.
    bool certain = !plan.parts.isEmpty();
    const PlacedPart *previous = nullptr;
    for (const PlacedPart &placed : std::as_const(plan.parts)) {
        if (!placed.included)
            continue;
        if (!placed.hasStart) {
            certain = false;
            break;
        }
        if (previous && previous->startRoman == placed.startRoman) {
            const int expected = previous->start + previous->part.pageCount;
            if (placed.start < expected || placed.start > expected + 1) {
                certain = false;
                break;
            }
        }
        previous = &placed;
    }
    plan.certain = certain;
    return plan;
}

QString partTitle(const ZipPart &part)
{
    const QString own = ownPartOfName(part.name);
    QString label;
    QString rest;
    const qsizetype dashes = own.indexOf(QStringLiteral("---"));
    if (dashes > 0) {
        label = wordsOf(own.left(dashes));
        rest = wordsOf(own.mid(dashes + 3));
    } else {
        rest = wordsOf(own);
    }

    const QString heading = part.heading.simplified().left(200);
    static const QRegularExpression untitled(QStringLiteral("^no title available\\b"), QRegularExpression::CaseInsensitiveOption);
    if (rest.isEmpty() || untitled.match(rest).hasMatch()) {
        if (!part.title.simplified().isEmpty())
            rest = part.title.simplified().left(200);
        else if (!heading.isEmpty())
            rest = heading;
        else
            rest = QStringLiteral("Untitled part");
    } else if (!heading.isEmpty()) {
        // A name cut short, "Requirements-of-network-syste", is finished from the heading on its first page. The name
        // has spaces for the heading's hyphens, so they're compared as one, and the heading's own spelling is kept.
        QString loose = heading;
        loose.replace(u'-', u' ');
        const qsizetype at = loose.indexOf(rest, 0, Qt::CaseInsensitive);
        if (at >= 0) {
            const QString found = heading.mid(at, rest.size());
            const QString remainder = heading.mid(at + rest.size()).section(u' ', 0, 0);
            if (found == found.toUpper())
                rest += remainder.toLower(); // A heading in capitals: the name's own casing reads better.
            else
                rest = found + remainder;
        }
    }
    if (!rest.isEmpty())
        rest[0] = rest.at(0).toUpper();
    if (label.isEmpty())
        return rest;
    label[0] = label.at(0).toUpper();
    return QStringLiteral("%1 – %2").arg(label, rest);
}

BookFromNames bookFromNames(const QStringList &names)
{
    static const QRegularExpression pattern(QStringLiteral("_((?:1[5-9]|20)\\d\\d)_(.+?)\\d?$"));
    QHash<int, int> years;
    QStringList titles;
    for (const QString &name : names) {
        const auto match = pattern.match(baseName(name));
        if (!match.hasMatch())
            continue;
        ++years[match.captured(1).toInt()];
        titles << wordsOf(match.captured(2));
    }
    BookFromNames book;
    if (titles.size() < 2)
        return book;
    // The names cut the title short by different amounts, so the longest one that the others begin is it.
    std::sort(titles.begin(), titles.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &candidate : std::as_const(titles)) {
        const auto agreeing = std::count_if(titles.cbegin(), titles.cend(), [&](const QString &other) {
            return candidate.startsWith(other, Qt::CaseInsensitive);
        });
        if (agreeing * 2 > titles.size()) {
            book.title = candidate;
            break;
        }
    }
    int bestCount = 0;
    for (auto it = years.constBegin(); it != years.constEnd(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            book.year = it.key();
        }
    }
    return book;
}

QJsonObject zipPartToJson(const ZipPart &part)
{
    QJsonArray numbers;
    for (const PrintedNumber &number : part.numbers)
        numbers.append(QJsonArray{number.page, number.value, number.roman});
    QJsonObject object{{QStringLiteral("name"), part.name},
                       {QStringLiteral("size"), part.size},
                       {QStringLiteral("sha256"), part.sha256},
                       {QStringLiteral("pages"), part.pageCount},
                       {QStringLiteral("numbers"), numbers}};
    if (!part.problem.isEmpty())
        object.insert(QStringLiteral("problem"), part.problem);
    if (!part.title.isEmpty())
        object.insert(QStringLiteral("title"), part.title);
    if (!part.heading.isEmpty())
        object.insert(QStringLiteral("heading"), part.heading);
    return object;
}

ZipPart zipPartFromJson(const QJsonObject &object)
{
    ZipPart part;
    part.name = object.value(QStringLiteral("name")).toString();
    part.size = object.value(QStringLiteral("size")).toInteger();
    part.sha256 = object.value(QStringLiteral("sha256")).toString();
    part.pageCount = object.value(QStringLiteral("pages")).toInt();
    part.problem = object.value(QStringLiteral("problem")).toString();
    part.title = object.value(QStringLiteral("title")).toString();
    part.heading = object.value(QStringLiteral("heading")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("numbers")).toArray()) {
        const QJsonArray triple = value.toArray();
        part.numbers << PrintedNumber{triple.at(0).toInt(), triple.at(1).toInt(), triple.at(2).toBool()};
    }
    return part;
}

QJsonArray planToJson(const StitchPlan &plan)
{
    QJsonArray array;
    for (const PlacedPart &placed : plan.parts) {
        QJsonObject object = zipPartToJson(placed.part);
        object.insert(QStringLiteral("included"), placed.included);
        object.insert(QStringLiteral("reason"), placed.reason);
        object.insert(QStringLiteral("placed"), placed.placed);
        if (placed.hasStart) {
            object.insert(QStringLiteral("start"), placed.start);
            object.insert(QStringLiteral("startRoman"), placed.startRoman);
        }
        array.append(object);
    }
    return array;
}

StitchPlan planFromJson(const QJsonArray &array)
{
    StitchPlan plan;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        PlacedPart placed;
        placed.part = zipPartFromJson(object);
        placed.included = object.value(QStringLiteral("included")).toBool(true);
        placed.reason = object.value(QStringLiteral("reason")).toString();
        placed.placed = object.value(QStringLiteral("placed")).toBool(true);
        placed.hasStart = object.contains(QStringLiteral("start"));
        placed.start = object.value(QStringLiteral("start")).toInt();
        placed.startRoman = object.value(QStringLiteral("startRoman")).toBool();
        if (placed.included && !placed.placed)
            ++plan.unplaced;
        plan.parts << placed;
    }
    return plan;
}

QByteArray orderToJson(const QList<OrderedPart> &parts)
{
    QJsonArray array;
    for (const OrderedPart &part : parts) {
        array.append(QJsonObject{{QStringLiteral("name"), part.name},
                                 {QStringLiteral("sha256"), part.sha256},
                                 {QStringLiteral("pages"), part.pageCount},
                                 {QStringLiteral("included"), part.included},
                                 {QStringLiteral("title"), part.title}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("parts"), array}})
        .toJson(QJsonDocument::Compact);
}

QList<OrderedPart> orderFromJson(const QByteArray &json)
{
    QList<OrderedPart> parts;
    const QJsonArray array = QJsonDocument::fromJson(json).object().value(QStringLiteral("parts")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        OrderedPart part;
        part.name = object.value(QStringLiteral("name")).toString();
        part.sha256 = object.value(QStringLiteral("sha256")).toString();
        part.pageCount = object.value(QStringLiteral("pages")).toInt();
        part.included = object.value(QStringLiteral("included")).toBool(true);
        part.title = object.value(QStringLiteral("title")).toString();
        if (!part.name.isEmpty())
            parts << part;
    }
    return parts;
}

QList<OrderedPart> orderedParts(const StitchPlan &plan)
{
    QList<OrderedPart> parts;
    for (const PlacedPart &placed : plan.parts) {
        OrderedPart part;
        part.name = placed.part.name;
        part.sha256 = placed.part.sha256;
        part.pageCount = placed.part.pageCount;
        part.included = placed.included;
        part.title = partTitle(placed.part);
        parts << part;
    }
    return parts;
}

QString orderKey(const QList<OrderedPart> &parts)
{
    QJsonArray array;
    for (const OrderedPart &part : parts) {
        if (part.included)
            array.append(QJsonArray{part.name, part.sha256, part.title});
    }
    const QByteArray json = QJsonDocument(array).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex().left(12));
}

namespace {

QString withoutSuffix(const QString &convertedPath)
{
    const QFileInfo info(convertedPath);
    return info.dir().filePath(info.completeBaseName());
}

} // namespace

QString orderPathFor(const QString &convertedPath)
{
    return withoutSuffix(convertedPath) + QStringLiteral(".order.json");
}

QString stitchedPathFor(const QString &convertedPath, const QList<OrderedPart> &parts)
{
    return withoutSuffix(convertedPath) + u'-' + orderKey(parts) + QStringLiteral(".pdf");
}

bool partOfPage(const QList<OrderedPart> &parts, int page, QString &partName, int &pageInPart)
{
    if (page < 0)
        return false;
    int start = 0;
    for (const OrderedPart &part : parts) {
        if (!part.included)
            continue;
        if (page < start + part.pageCount) {
            partName = part.name;
            pageInPart = page - start;
            return true;
        }
        start += part.pageCount;
    }
    return false;
}

int pageOfPart(const QList<OrderedPart> &parts, const QString &partName, int pageInPart)
{
    int start = 0;
    for (const OrderedPart &part : parts) {
        if (!part.included)
            continue;
        if (part.name == partName)
            return pageInPart >= 0 && pageInPart < part.pageCount ? start + pageInPart : -1;
        start += part.pageCount;
    }
    return -1;
}

QList<int> pageMapping(const QList<OrderedPart> &from, const QList<OrderedPart> &to)
{
    QList<int> mapping;
    for (const OrderedPart &part : from) {
        if (!part.included)
            continue;
        for (int page = 0; page < part.pageCount; ++page)
            mapping << pageOfPart(to, part.name, page);
    }
    return mapping;
}

bool writeOrderFile(const QString &convertedPath, const QList<OrderedPart> &parts)
{
    const QString path = orderPathFor(convertedPath);
    const QByteArray json = orderToJson(parts);
    {
        QFile existing(path);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == json)
            return true;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
}

} // namespace papertrail::import
