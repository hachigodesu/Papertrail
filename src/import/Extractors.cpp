#include "import/Extractors.h"

#include "core/model/Identifiers.h"

#include <QDate>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstring>

namespace papertrail::import {

using core::PersonName;
using core::PublicationType;

void EmbeddedMetadata::fillGapsFrom(const EmbeddedMetadata &other)
{
    if (title.isEmpty()) title = other.title;
    if (authors.isEmpty()) authors = other.authors;
    if (publisher.isEmpty()) publisher = other.publisher;
    if (date.isEmpty()) date = other.date;
    if (language.isEmpty()) language = other.language;
    if (doi.isEmpty()) doi = other.doi;
    if (isbn.isEmpty()) isbn = other.isbn;
    if (issn.isEmpty()) issn = other.issn;
    if (description.isEmpty()) description = other.description;
    if (keywords.isEmpty()) keywords = other.keywords;
}

void EmbeddedMetadata::decodeEntities()
{
    title = decodeHtmlEntities(title);
    publisher = decodeHtmlEntities(publisher);
    description = decodeHtmlEntities(description);
    keywords = decodeHtmlEntities(keywords);
    for (QString &author : authors)
        author = decodeHtmlEntities(author);
}

QString decodeHtmlEntities(const QString &text)
{
    if (!text.contains(u'&'))
        return text;

    static const QHash<QString, QString> named = {
        {QStringLiteral("amp"), QStringLiteral("&")},    {QStringLiteral("lt"), QStringLiteral("<")},
        {QStringLiteral("gt"), QStringLiteral(">")},     {QStringLiteral("quot"), QStringLiteral("\"")},
        {QStringLiteral("apos"), QStringLiteral("'")},   {QStringLiteral("quest"), QStringLiteral("?")},
        {QStringLiteral("nbsp"), QStringLiteral(" ")},   {QStringLiteral("ndash"), QStringLiteral("–")},
        {QStringLiteral("mdash"), QStringLiteral("—")},  {QStringLiteral("rsquo"), QStringLiteral("’")},
        {QStringLiteral("lsquo"), QStringLiteral("‘")},  {QStringLiteral("rdquo"), QStringLiteral("”")},
        {QStringLiteral("ldquo"), QStringLiteral("“")},  {QStringLiteral("colon"), QStringLiteral(":")},
        {QStringLiteral("comma"), QStringLiteral(",")},  {QStringLiteral("period"), QStringLiteral(".")},
        {QStringLiteral("excl"), QStringLiteral("!")},   {QStringLiteral("percnt"), QStringLiteral("%")},
    };
    static const QRegularExpression entity(QStringLiteral("&(#[xX][0-9a-fA-F]{1,6}|#[0-9]{1,7}|[a-zA-Z]{2,8});"));

    QString result;
    qsizetype last = 0;
    auto matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += text.mid(last, match.capturedStart() - last);
        const QString code = match.captured(1);
        QString replacement = match.captured(0); // Anything unrecognised stays as it was.
        if (code.startsWith(u'#')) {
            bool ok = false;
            const bool hex = code.size() > 1 && (code.at(1) == u'x' || code.at(1) == u'X');
            const uint value = code.mid(hex ? 2 : 1).toUInt(&ok, hex ? 16 : 10);
            const bool validCodePoint = ok && value > 0 && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF);
            if (validCodePoint) {
                const char32_t character = value;
                replacement = QString::fromUcs4(&character, 1);
            }
        } else if (const auto found = named.constFind(code); found != named.constEnd()) {
            replacement = *found;
        }
        result += replacement;
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

namespace {

// Identifiers often appear as "doi:10.x", "urn:isbn:978..." or a URL, so these find them in any
// of those forms.
void takeIdentifier(EmbeddedMetadata &metadata, const QString &value)
{
    if (metadata.doi.isEmpty())
        metadata.doi = findDoi(value);
    if (metadata.isbn.isEmpty()) {
        const QString digits = core::normaliseIsbn(value.section(u':', -1));
        if (isValidIsbn(digits))
            metadata.isbn = digits;
    }
}

} // namespace

EmbeddedMetadata parseXmp(const QByteArray &xmp)
{
    EmbeddedMetadata metadata;

    const auto apply = [&metadata](const QString &property, const QString &value) {
        if (property == QStringLiteral("dc:title") && metadata.title.isEmpty()) {
            metadata.title = value;
        } else if (property == QStringLiteral("dc:creator")) {
            metadata.authors << value;
        } else if (property == QStringLiteral("dc:description") && metadata.description.isEmpty()) {
            metadata.description = value;
        } else if (property == QStringLiteral("dc:subject")) {
            metadata.keywords += (metadata.keywords.isEmpty() ? QString() : QStringLiteral(", ")) + value;
        } else if (property == QStringLiteral("dc:language") && metadata.language.isEmpty()) {
            metadata.language = value;
        } else if (property == QStringLiteral("dc:publisher") && metadata.publisher.isEmpty()) {
            metadata.publisher = value;
        } else if ((property == QStringLiteral("dc:date") || property == QStringLiteral("prism:publicationDate")
                    || property == QStringLiteral("prism:coverDate")) && metadata.date.isEmpty()) {
            metadata.date = value;
        } else if (property == QStringLiteral("prism:doi") || property == QStringLiteral("pdfx:doi")
                   || property == QStringLiteral("crossmark:DOI") || property == QStringLiteral("dc:identifier")) {
            takeIdentifier(metadata, value);
        } else if (property == QStringLiteral("prism:isbn") || property == QStringLiteral("prism:eIssn")
                   || property == QStringLiteral("prism:issn")) {
            if (property.endsWith(QStringLiteral("isbn")))
                takeIdentifier(metadata, value);
            else if (metadata.issn.isEmpty())
                metadata.issn = core::normaliseIssn(value);
        } else if (property == QStringLiteral("pdf:Keywords") && metadata.keywords.isEmpty()) {
            metadata.keywords = value;
        }
    };

    // XMP can hold a value as an attribute, as an element's text, or in a nested rdf:li. So each
    // piece of text is applied to whichever property element most recently opened.
    QXmlStreamReader reader(xmp);
    QString property;
    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.prefix() != QStringLiteral("rdf"))
                property = reader.prefix().toString() + u':' + reader.name().toString();
            if (reader.name() == QStringLiteral("Description")) {
                const auto attributes = reader.attributes();
                for (const QXmlStreamAttribute &attribute : attributes) {
                    if (attribute.prefix() != QStringLiteral("rdf"))
                        apply(attribute.prefix().toString() + u':' + attribute.name().toString(),
                              attribute.value().toString().trimmed());
                }
            }
        } else if (reader.isCharacters() && !reader.isWhitespace() && !property.isEmpty()) {
            const QString value = reader.text().toString().trimmed();
            if (!value.isEmpty())
                apply(property, value);
        }
    }
    return metadata;
}

EmbeddedMetadata parseOpf(const QByteArray &opf)
{
    EmbeddedMetadata metadata;
    QXmlStreamReader reader(opf);
    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.prefix() != QStringLiteral("dc"))
            continue;

        const QString name = reader.name().toString();
        const QString value = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        if (value.isEmpty())
            continue;

        if (name == QStringLiteral("title") && metadata.title.isEmpty())
            metadata.title = value;
        else if (name == QStringLiteral("creator"))
            metadata.authors << value;
        else if (name == QStringLiteral("publisher") && metadata.publisher.isEmpty())
            metadata.publisher = value;
        else if (name == QStringLiteral("date") && metadata.date.isEmpty())
            metadata.date = value;
        else if (name == QStringLiteral("language") && metadata.language.isEmpty())
            metadata.language = value;
        else if (name == QStringLiteral("description") && metadata.description.isEmpty())
            metadata.description = value;
        else if (name == QStringLiteral("subject"))
            metadata.keywords += (metadata.keywords.isEmpty() ? QString() : QStringLiteral(", ")) + value;
        else if (name == QStringLiteral("identifier"))
            takeIdentifier(metadata, value);
    }
    return metadata;
}

EmbeddedMetadata parsePdfInfo(const QString &title, const QString &author, const QString &subject,
                              const QString &keywords)
{
    EmbeddedMetadata metadata;
    metadata.title = title.trimmed();
    for (const QString &name : splitAuthorList(author))
        metadata.authors << name;
    metadata.description = subject.trimmed();
    metadata.keywords = keywords.trimmed();
    metadata.doi = findDoi(subject + u' ' + keywords);
    return metadata;
}

namespace {

quint32 readBe32(const char *data)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return (quint32(bytes[0]) << 24) | (quint32(bytes[1]) << 16) | (quint32(bytes[2]) << 8) | quint32(bytes[3]);
}

quint16 readBe16(const char *data)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return quint16((quint32(bytes[0]) << 8) | quint32(bytes[1]));
}

} // namespace

KindleHeader parseKindleHeader(const QByteArray &head)
{
    KindleHeader header;
    if (head.size() < 80 || std::memcmp(head.constData() + 60, "BOOKMOBI", 8) != 0)
        return header;
    header.isKindleFile = true;

    const quint32 recordCount = readBe16(head.constData() + 76);
    if (recordCount == 0 || head.size() < 78 + 8)
        return header;
    const qsizetype record0 = readBe32(head.constData() + 78);

    // PalmDOC header (16 bytes), then the MOBI header. All of this comes from an untrusted file,
    // so every offset is checked against what was actually read.
    constexpr qsizetype mobiOffset = 16;
    if (record0 < 0 || record0 + mobiOffset + 132 > head.size())
        return header;
    const char *mobi = head.constData() + record0 + mobiOffset;
    if (std::memcmp(mobi, "MOBI", 4) != 0)
        return header;
    // The PalmDOC header's encryption type.
    header.isEncrypted = (quint8(head.at(record0 + 12)) << 8 | quint8(head.at(record0 + 13))) != 0;

    const qsizetype mobiLength = readBe32(mobi + 4);
    const quint32 version = readBe32(mobi + 20);
    const quint32 flags = readBe32(mobi + 112);
    header.isKf8 = version == 8;

    const qsizetype exth = record0 + mobiOffset + mobiLength;
    if (!(flags & 0x40) || mobiLength <= 0 || exth + 12 > head.size())
        return header;
    if (std::memcmp(head.constData() + exth, "EXTH", 4) != 0)
        return header;

    const quint32 entries = readBe32(head.constData() + exth + 8);
    qsizetype position = exth + 12;
    for (quint32 i = 0; i < entries && position + 8 <= head.size(); ++i) {
        const quint32 kind = readBe32(head.constData() + position);
        const qsizetype length = readBe32(head.constData() + position + 4);
        if (length < 8 || position + length > head.size())
            break;
        const QByteArray bytes = head.mid(position + 8, length - 8);
        const QString value = QString::fromUtf8(bytes).trimmed();
        switch (kind) {
        case 100: header.metadata.authors << value; break;
        case 101: header.metadata.publisher = value; break;
        case 103: header.metadata.description = value; break;
        case 104: takeIdentifier(header.metadata, value); break;
        case 105: header.metadata.keywords = value; break;
        case 106: header.metadata.date = value; break;
        case 503: header.metadata.title = value; break;
        case 524: header.metadata.language = value; break;
        case 121: header.isKf8 = true; break; // A joint file: old MOBI data followed by KF8.
        default: break;
        }
        position += length;
    }
    return header;
}

QString findDoi(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("\\b10\\.\\d{4,9}/[^\\s\"<>]+"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return {};
    QString doi = match.captured(0);
    // Sentence punctuation after a DOI isn't part of it.
    while (!doi.isEmpty() && QStringLiteral(".,;:)]}'").contains(doi.back()))
        doi.chop(1);
    return core::normaliseDoi(doi);
}

QString articleDoiOnPage(const QString &pageText)
{
    const QStringList dois = doisOnPage(pageText);
    return dois.size() == 1 ? dois.first() : QString();
}

QStringList doisOnPage(const QString &pageText)
{
    QString text = pageText;
    // A heading such as "References" or "7. Bibliography". Text extraction often joins it to the line before
    // or after, so it counts at either end of a line.
    static const QRegularExpression references(
        QStringLiteral("^\\s*(?:\\d+\\.?\\s*)?(?:references|bibliography)\\b|\\b(?:references|bibliography)\\s*$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    const QRegularExpressionMatch heading = references.match(text);
    if (heading.hasMatch())
        text.truncate(heading.capturedStart());

    static const QRegularExpression pattern(QStringLiteral("\\b10\\.\\d{4,9}/[^\\s\"<>]+"));
    QStringList found;
    for (auto it = pattern.globalMatch(text); it.hasNext();) {
        const QString doi = findDoi(it.next().captured(0));
        if (!doi.isEmpty() && !found.contains(doi))
            found << doi;
    }
    return found;
}

namespace {

std::optional<int> monthFromName(const QString &name)
{
    static const QStringList months = {QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                       QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                       QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                       QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    const qsizetype index = months.indexOf(name.left(3).toLower());
    return index < 0 ? std::nullopt : std::optional<int>(int(index) + 1);
}

std::optional<int> printedYear(const QString &digits)
{
    const int year = digits.toInt();
    return year >= 1600 && year <= 2200 ? std::optional<int>(year) : std::nullopt;
}

} // namespace

std::optional<IssueNumbering> findIssueNumbering(const QString &text)
{
    static const QString month = QStringLiteral(
        "(jan(?:uary)?|feb(?:ruary)?|mar(?:ch)?|apr(?:il)?|may|june?|july?|aug(?:ust)?|sep(?:t(?:ember)?)?|oct(?:ober)?|"
        "nov(?:ember)?|dec(?:ember)?)\\.?");
    // "VOL. 7, NO. 11, NOVEMBER 2026", as in a running header, with the date optional.
    static const QRegularExpression after(
        QStringLiteral("\\bvol(?:ume)?\\.?\\s*(\\d{1,4})\\s*,?\\s*(?:no|number|issue)\\.?\\s*(\\d{1,4})\\b(?:\\s*,?\\s*")
            + month + QStringLiteral("\\s*(\\d{4}))?"),
        QRegularExpression::CaseInsensitiveOption);
    // "NOVEMBER 2026 VOLUME 7 NUMBER 11", as on some covers.
    static const QRegularExpression before(
        QStringLiteral("\\b") + month
            + QStringLiteral("\\s*(\\d{4})\\W{0,6}vol(?:ume)?\\.?\\s*(\\d{1,4})\\W{0,6}(?:no|number|issue)\\.?\\s*(\\d{1,4})\\b"),
        QRegularExpression::CaseInsensitiveOption);

    const QString flat = text.simplified();
    if (const QRegularExpressionMatch match = before.match(flat); match.hasMatch()) {
        IssueNumbering found;
        found.month = monthFromName(match.captured(1));
        found.year = printedYear(match.captured(2));
        found.volume = match.captured(3);
        found.issue = match.captured(4);
        return found;
    }
    if (const QRegularExpressionMatch match = after.match(flat); match.hasMatch()) {
        IssueNumbering found;
        found.volume = match.captured(1);
        found.issue = match.captured(2);
        if (!match.captured(3).isEmpty()) {
            found.month = monthFromName(match.captured(3));
            found.year = printedYear(match.captured(4));
        }
        return found;
    }
    return std::nullopt;
}

bool isValidIsbn(const QString &digits)
{
    if (digits.size() == 10) {
        int sum = 0;
        for (int i = 0; i < 10; ++i) {
            const QChar c = digits.at(i);
            int value;
            if (c == u'X' && i == 9)
                value = 10;
            else if (c.isDigit())
                value = c.digitValue();
            else
                return false;
            sum += value * (10 - i);
        }
        return sum % 11 == 0;
    }
    if (digits.size() == 13) {
        if (!digits.startsWith(QStringLiteral("978")) && !digits.startsWith(QStringLiteral("979")))
            return false;
        int sum = 0;
        for (int i = 0; i < 13; ++i) {
            if (!digits.at(i).isDigit())
                return false;
            sum += digits.at(i).digitValue() * (i % 2 == 0 ? 1 : 3);
        }
        return sum % 10 == 0;
    }
    return false;
}

QString findIsbn(const QString &text)
{
    // Near the word ISBN, either length is believable. Elsewhere only the long form with its
    // 978 or 979 prefix is, because ten digits on their own could be anything.
    static const QRegularExpression labelled(
        QStringLiteral("ISBN(?:-1[03])?(?:\\s*\\([^)]*\\))?[:\\s]*((?:\\d[-\\s]?){9}[\\dXx](?:[-\\s]?\\d){0,3})"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = labelled.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString digits = core::normaliseIsbn(match.captured(1));
        if (isValidIsbn(digits))
            return digits;
        // The match can run into a following number, so also try just the first ten digits.
        if (digits.size() > 10 && isValidIsbn(digits.left(10)))
            return digits.left(10);
    }

    static const QRegularExpression bare(QStringLiteral("\\b97[89][-\\s]?(?:\\d[-\\s]?){9}\\d\\b"));
    auto bareMatches = bare.globalMatch(text);
    while (bareMatches.hasNext()) {
        const QString digits = core::normaliseIsbn(bareMatches.next().captured(0));
        if (isValidIsbn(digits))
            return digits;
    }
    return {};
}

QString findIssn(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("ISSN[:\\s(a-zA-Z)]*(\\d{4}-\\d{3}[\\dXx])"),
                                            QRegularExpression::CaseInsensitiveOption);
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QString issn = matches.next().captured(1).toUpper();
        int sum = 0;
        int weight = 8;
        for (const QChar c : issn) {
            if (c == u'-')
                continue;
            sum += (c == u'X' ? 10 : c.digitValue()) * weight--;
        }
        if (sum % 11 == 0)
            return issn;
    }
    return {};
}

QString findArxivId(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("arXiv:\\s*(\\d{4}\\.\\d{4,5}(?:v\\d+)?|[a-z-]+(?:\\.[A-Z]{2})?/\\d{7}(?:v\\d+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? core::normaliseArxivId(match.captured(1)) : QString();
}

namespace {

std::optional<int> plausibleYear(int year)
{
    if (year < 1400 || year > QDate::currentDate().year() + 1)
        return std::nullopt;
    return year;
}

} // namespace

std::optional<int> findCopyrightYear(const QString &text)
{
    // In order of trust. A day and month can come first: "Published: 12 May 2021".
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral(
            "(?:©|\\(c\\)|copyright)[^\\n\\d]{0,20}((?:1[5-9]|20)\\d\\d)\\b"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(
            "(?:published|available online|first published)(?:\\s+(?:in|online|on))?[:\\s]*(?:\\d{1,2}\\s+)?[^\\n\\d]{0,20}?"
            "((?:1[5-9]|20)\\d\\d)\\b"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(
            "(?:accepted|received)[:\\s]*(?:\\d{1,2}\\s+)?[^\\n\\d]{0,20}?((?:1[5-9]|20)\\d\\d)\\b"),
            QRegularExpression::CaseInsensitiveOption),
    };
    for (const QRegularExpression &pattern : patterns) {
        const QRegularExpressionMatch match = pattern.match(text);
        if (match.hasMatch()) {
            if (const auto year = plausibleYear(match.captured(1).toInt()))
                return year;
        }
    }
    return std::nullopt;
}

std::optional<int> yearFromDate(const QString &date)
{
    static const QRegularExpression pattern(QStringLiteral("\\b(1[4-9]|20)\\d\\d\\b"));
    const QRegularExpressionMatch match = pattern.match(date);
    return match.hasMatch() ? plausibleYear(match.captured(0).toInt()) : std::nullopt;
}

bool isJunkTitle(const QString &title)
{
    const QString lower = title.trimmed().toLower();
    if (lower.size() < 4)
        return true;

    static const QStringList exact = {
        QStringLiteral("untitled"), QStringLiteral("unknown"), QStringLiteral("none"), QStringLiteral("title"),
        QStringLiteral("document"), QStringLiteral("no title"), QStringLiteral("slide 1"),
        QStringLiteral("powerpoint presentation"), QStringLiteral("article"), QStringLiteral("default"),
    };
    if (exact.contains(lower))
        return true;

    // Software names, file names, and the codes publishers' production systems leave behind, such
    // as "ab1c00123 1..10", "jabc-12-345 1140..1146" and "tx_1~abs:at/htc:os:tx_2~abs:at//aff:at".
    static const QRegularExpression patterns(
        QStringLiteral("^(microsoft (word|powerpoint|excel)\\b|untitled\\b|document\\d*$|doi:|http|www\\.|file:|"
                       "c:\\\\|/|\\d+$|[a-z0-9_]+\\d+$)|\\.(pdf|docx?|tex|dvi|ps|indd|qxd|fm|rtf|pptx?|xml|txt|cdr|eps)$|"
                       "\\s\\d+\\.\\.\\d+$|[~|^]|//|\\bcover$"));
    if (patterns.match(lower).hasMatch())
        return true;

    // One long unbroken token full of symbols is a code rather than a title.
    return !lower.contains(u' ') && lower.size() > 12
        && std::any_of(lower.begin(), lower.end(), [](QChar c) { return c == u'_' || c == u':' || c == u'/'; });
}

bool looksLikeFullTitle(const QString &title)
{
    // Titles in languages written without spaces can't be counted in words, so use characters.
    const auto nonLatin = std::count_if(title.begin(), title.end(), [](QChar c) { return c.unicode() >= 0x2E80; });
    if (nonLatin >= 6)
        return true;
    return title.simplified().count(u' ') >= 2 && title.size() >= 15;
}

bool isJunkAuthor(const QString &author)
{
    const QString lower = author.trimmed().toLower();
    if (lower.size() < 2)
        return true;
    static const QStringList exact = {
        QStringLiteral("administrator"), QStringLiteral("admin"), QStringLiteral("user"), QStringLiteral("owner"),
        QStringLiteral("unknown"), QStringLiteral("author"), QStringLiteral("default"), QStringLiteral("none"),
        QStringLiteral("anonymous"), QStringLiteral("root"), QStringLiteral("microsoft"), QStringLiteral("guest"),
    };
    if (exact.contains(lower))
        return true;
    static const QRegularExpression patterns(
        QStringLiteral("^(microsoft|adobe|acrobat|latex|pdftex|ghostscript|writer)\\b|@|\\.(com|org|net)\\b|\\d{3}"));
    return patterns.match(lower).hasMatch();
}

QString titleFromFileName(const QString &fileName)
{
    QString name = QFileInfo(fileName).completeBaseName();
    name.replace(QRegularExpression(QStringLiteral("[_.]+")), QStringLiteral(" "));
    name.replace(QRegularExpression(QStringLiteral("(?<=\\S)-(?=\\S)")), QStringLiteral(" "));
    name = name.simplified();
    return name.isEmpty() ? QFileInfo(fileName).fileName() : name;
}

PersonName parsePersonName(const QString &name)
{
    const QString cleaned = name.simplified();
    if (cleaned.contains(u',')) {
        const qsizetype comma = cleaned.indexOf(u',');
        return {cleaned.left(comma).trimmed(), cleaned.mid(comma + 1).trimmed()};
    }

    QStringList words = cleaned.split(u' ', Qt::SkipEmptyParts);
    if (words.size() <= 1)
        return {cleaned, QString()};

    // Family names start at the last word, or earlier if lowercase particles lead into it.
    static const QStringList particles = {
        QStringLiteral("van"), QStringLiteral("von"), QStringLiteral("der"), QStringLiteral("den"),
        QStringLiteral("de"), QStringLiteral("del"), QStringLiteral("della"), QStringLiteral("di"),
        QStringLiteral("da"), QStringLiteral("du"), QStringLiteral("la"), QStringLiteral("le"),
        QStringLiteral("ten"), QStringLiteral("ter"), QStringLiteral("bin"), QStringLiteral("ibn"),
    };
    qsizetype familyStart = words.size() - 1;
    while (familyStart > 1 && particles.contains(words.at(familyStart - 1)))
        --familyStart;
    return {words.mid(familyStart).join(u' '), words.mid(0, familyStart).join(u' ')};
}

QStringList splitAuthorList(const QString &authors)
{
    static const QRegularExpression separators(QStringLiteral("\\s*(?:;|/|\\band\\b|&|\\n)\\s*"));
    QStringList people;
    for (const QString &part : authors.split(separators, Qt::SkipEmptyParts)) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty())
            continue;

        // Commas are ambiguous. "Smith, Jane" is one person, but "Jane Smith, Omar Haddad" is two.
        const QStringList pieces = trimmed.split(u',', Qt::SkipEmptyParts);
        const bool oneFamilyName = pieces.size() == 2 && !pieces.at(0).trimmed().contains(u' ');
        if (pieces.size() >= 2 && !oneFamilyName) {
            for (const QString &piece : pieces) {
                if (!piece.trimmed().isEmpty())
                    people << piece.trimmed();
            }
        } else {
            people << trimmed;
        }
    }
    return people;
}

namespace {

bool containsAny(const QString &text, const QStringList &needles)
{
    return std::any_of(needles.begin(), needles.end(), [&](const QString &needle) { return text.contains(needle); });
}

// A name with words in capitals, as headings set them, the way a title has it: "References re SAMPLE ACT, 2018"
// reads "References re Sample Act, 2018". A word of one letter, such as "R.", is left as it is.
QString fromCapitals(const QString &name)
{
    static const QStringList small = {QStringLiteral("v"), QStringLiteral("v."), QStringLiteral("of"), QStringLiteral("and"),
                                      QStringLiteral("the"), QStringLiteral("for"), QStringLiteral("in"), QStringLiteral("on"),
                                      QStringLiteral("re"), QStringLiteral("et"), QStringLiteral("al.")};
    QStringList words = name.split(u' ', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < words.size(); ++i) {
        QString &word = words[i];
        const auto letters = std::count_if(word.cbegin(), word.cend(), [](QChar c) { return c.isLetter(); });
        if (letters < 2 || word != word.toUpper())
            continue;
        word = word.toLower();
        if (i > 0 && small.contains(word))
            continue;
        const auto first = std::find_if(word.begin(), word.end(), [](QChar c) { return c.isLetter(); });
        *first = first->toUpper();
    }
    return words.join(u' ');
}

} // namespace

// This never answers "preprint". Published papers carry too much of the same wording and the same
// identifiers, such as an arXiv number or "accepted for publication", so that's for the user to say.
std::optional<CaseDetails> caseDetailsOnFirstPage(const QString &firstPage, const QString &title, const QString &laterPages)
{
    const QString text = firstPage.left(6000);
    const QString lower = text.toLower();
    // An article about a case has an abstract, keywords or a DOI on its first page; a judgment doesn't.
    if (containsAny(lower, {QStringLiteral("abstract"), QStringLiteral("keywords"), QStringLiteral("doi.org"),
                            QStringLiteral("doi:")})) {
        return std::nullopt;
    }

    CaseDetails details;
    struct UkCourt {
        const char *code;
        const char *court;
        const char *jurisdiction;
    };
    static const UkCourt ukCourts[] = {
        {"UKSC", "Supreme Court of the United Kingdom", "United Kingdom"},
        {"UKHL", "House of Lords", "United Kingdom"},
        {"UKPC", "Privy Council", "United Kingdom"},
        {"EWCA Civ", "Court of Appeal (Civil Division)", "England and Wales"},
        {"EWCA Crim", "Court of Appeal (Criminal Division)", "England and Wales"},
        {"EWHC", "High Court", "England and Wales"},
        {"EWFC", "Family Court", "England and Wales"},
        {"EWCOP", "Court of Protection", "England and Wales"},
        {"UKUT", "Upper Tribunal", "United Kingdom"},
        {"UKFTT", "First-tier Tribunal", "United Kingdom"},
        {"CSIH", "Court of Session, Inner House", "Scotland"},
        {"CSOH", "Court of Session, Outer House", "Scotland"},
        {"NICA", "Court of Appeal in Northern Ireland", "Northern Ireland"},
        {"IESC", "Supreme Court of Ireland", "Ireland"},
        {"HCA", "High Court of Australia", "Australia"},
        {"NZSC", "Supreme Court of New Zealand", "New Zealand"},
    };
    static const QRegularExpression neutral(
        QStringLiteral("\\[((?:19|20)\\d\\d)\\]\\s+(UKSC|UKHL|UKPC|EWCA\\s+Civ|EWCA\\s+Crim|EWHC|EWFC|EWCOP|UKUT|UKFTT|CSIH|CSOH|NICA|"
                       "IESC|HCA|NZSC)\\s+(\\d{1,5})(?:\\s*\\((?:[A-Za-z]+)\\))?"));
    const QRegularExpressionMatch neutralMatch = neutral.match(text);
    // Canada's neutral citations have no brackets, such as "2022 SCC 30", and the EU's courts give an ECLI.
    struct CanadianCourt {
        const char *code;
        const char *court;
    };
    static const CanadianCourt canadianCourts[] = {
        {"SCC", "Supreme Court of Canada"},        {"FCA", "Federal Court of Appeal"},
        {"FC", "Federal Court"},                   {"TCC", "Tax Court of Canada"},
        {"ONCA", "Court of Appeal for Ontario"},   {"ONSC", "Ontario Superior Court of Justice"},
        {"BCCA", "Court of Appeal for British Columbia"}, {"BCSC", "Supreme Court of British Columbia"},
        {"ABCA", "Court of Appeal of Alberta"},    {"ABKB", "Court of King's Bench of Alberta"},
        {"ABQB", "Court of Queen's Bench of Alberta"}, {"QCCA", "Court of Appeal of Quebec"},
        {"QCCS", "Superior Court of Quebec"},      {"MBCA", "Court of Appeal of Manitoba"},
        {"SKCA", "Court of Appeal for Saskatchewan"}, {"NSCA", "Nova Scotia Court of Appeal"},
        {"NSSC", "Supreme Court of Nova Scotia"},  {"NBCA", "Court of Appeal of New Brunswick"},
        {"NLCA", "Court of Appeal of Newfoundland and Labrador"}, {"PECA", "Court of Appeal of Prince Edward Island"},
    };
    static const QRegularExpression canadian(
        QStringLiteral("\\b((?:19|20)\\d\\d)\\s+(SCC|FCA|FC|TCC|ONCA|ONSC|BCCA|BCSC|ABCA|ABKB|ABQB|QCCA|QCCS|MBCA|SKCA|NSCA|"
                       "NSSC|NBCA|NLCA|PECA)\\s+(\\d{1,6})\\b"));
    static const QRegularExpression ecli(QStringLiteral("\\bECLI:EU:([CT]):((?:19|20)\\d\\d):(\\d{1,6})\\b"));
    const QRegularExpressionMatch canadianMatch = canadian.match(text);
    const QRegularExpressionMatch ecliMatch = ecli.match(text);
    // The Supreme Court Reports head each page with the report and its volume, "[2021] 1 S.C.R." (in French "R.C.S."),
    // and the page, with the parties below it.
    static const QRegularExpression scr(QStringLiteral("\\[((?:19|20)\\d\\d)\\]\\s+(\\d{1,2})\\s+(?:S\\.C\\.R\\.|R\\.C\\.S\\.)"));
    const QString firstLine = text.trimmed().section(u'\n', 0, 0).left(300);
    const QRegularExpressionMatch scrMatch = scr.match(firstLine);
    const bool scrReport = scrMatch.hasMatch()
        && containsAny(lower, {QStringLiteral("appellant"), QStringLiteral("respondent"), QStringLiteral("intervener"),
                               QStringLiteral("appelant"), QStringLiteral("intimé"), QStringLiteral("in the matter of"),
                               QStringLiteral("supreme court of canada")});
    bool courtHeading = false;
    // A Canadian or EU judgment's first page needn't name its parties as "X v Y" on a line of their own.
    bool nameOptional = false;
    if (neutralMatch.hasMatch()) {
        details.neutralCitation = neutralMatch.captured(0).simplified();
        details.year = neutralMatch.captured(1).toInt();
        const QString code = neutralMatch.captured(2).simplified();
        for (const UkCourt &court : ukCourts) {
            if (code == QLatin1StringView(court.code)) {
                details.court = QString::fromLatin1(court.court);
                details.jurisdiction = QString::fromLatin1(court.jurisdiction);
            }
        }
        // A neutral citation alone could be one cited. A judgment also says it's one, or who heard it.
        courtHeading = containsAny(lower, {QStringLiteral("judgment"), QStringLiteral("neutral citation"),
                                           QStringLiteral("before:"), QStringLiteral("before :"), QStringLiteral("lord justice"),
                                           QStringLiteral("lady justice"), QStringLiteral("mr justice"),
                                           QStringLiteral("mrs justice"), QStringLiteral("judgment given on")});
    } else if (scrReport
               || (canadianMatch.hasMatch()
                   && containsAny(lower, {QStringLiteral("supreme court of canada"), QStringLiteral("court of appeal for "),
                                          QStringLiteral("court of appeal of "), QStringLiteral("reasons for judgment"),
                                          QStringLiteral("coram"), QStringLiteral("court of king's bench"),
                                          QStringLiteral("court of queen's bench"), QStringLiteral("superior court of justice"),
                                          QStringLiteral("federal court"), QStringLiteral("tax court of canada")}))) {
        QRegularExpressionMatch citation = canadianMatch;
        if (scrReport) {
            details.court = QStringLiteral("Supreme Court of Canada");
            details.report = QStringLiteral("S.C.R.");
            details.reportVolume = QString::number(scrMatch.captured(2).toInt());
            details.year = scrMatch.captured(1).toInt();
            // The page is at the heading's far end from the report: after it on a right-hand page, before it on a left.
            QString heading = firstLine;
            heading.remove(scrMatch.capturedStart(0), scrMatch.capturedLength(0));
            static const QRegularExpression pageAtAnEnd(QStringLiteral("^\\s*(\\d{1,4})\\s|\\s(\\d{1,4})\\s*$"));
            if (const QRegularExpressionMatch page = pageAtAnEnd.match(heading); page.hasMatch())
                details.reportPage = page.captured(1).isEmpty() ? page.captured(2) : page.captured(1);

            // Its own name and neutral citation come a few pages on, after the parties: "Indexed as: R. v. Sample",
            // then "2021 SCC 99". The pages after that cite other cases, so only these are believed.
            const QString front = text + u'\n' + laterPages.left(40000);
            static const QRegularExpression indexedAs(
                QStringLiteral("Indexed\\s+as\\s*:\\s*(\\S.{1,300}?)\\s+((?:19|20)\\d\\d)\\s+SCC\\s+(\\d{1,4})\\b"),
                QRegularExpression::DotMatchesEverythingOption);
            const QRegularExpressionMatch indexed = indexedAs.match(front);
            if (indexed.hasMatch()) {
                details.name = fromCapitals(indexed.captured(1).simplified());
                citation = canadian.match(front, indexed.capturedStart(2));
                // Then when it was heard and decided, the decision last: "2020: September 22, 23; 2021: March 25."
                static const QRegularExpression heard(
                    QStringLiteral("((?:19|20)\\d\\d)\\s*:\\s*(January|February|March|April|May|June|July|August|September|"
                                   "October|November|December)\\s+(\\d{1,2})\\b"));
                const QString after = front.mid(indexed.capturedEnd(0), 400);
                if (const QRegularExpressionMatch first = heard.match(after); first.hasMatch()) {
                    const qsizetype lineEnd = after.indexOf(u'\n', first.capturedStart(0));
                    const QString line = after.mid(first.capturedStart(0), lineEnd < 0 ? -1 : lineEnd - first.capturedStart(0));
                    QRegularExpressionMatch last;
                    for (QRegularExpressionMatchIterator each = heard.globalMatch(line); each.hasNext();)
                        last = each.next();
                    static const QStringList months = {QStringLiteral("January"), QStringLiteral("February"), QStringLiteral("March"),
                                                       QStringLiteral("April"), QStringLiteral("May"), QStringLiteral("June"),
                                                       QStringLiteral("July"), QStringLiteral("August"), QStringLiteral("September"),
                                                       QStringLiteral("October"), QStringLiteral("November"), QStringLiteral("December")};
                    details.year = last.captured(1).toInt();
                    details.month = int(months.indexOf(last.captured(2))) + 1;
                    details.day = last.captured(3).toInt();
                }
            } else if (!citation.hasMatch() || citation.captured(2) != QLatin1StringView("SCC")
                       || std::abs(citation.captured(1).toInt() - *details.year) > 1) {
                citation = {}; // One on the first page that isn't this court's, or not from about then, is one it cites.
            }
        }
        if (citation.hasMatch()) {
            details.neutralCitation = citation.captured(0).simplified();
            if (!details.month)
                details.year = citation.captured(1).toInt();
            for (const CanadianCourt &court : canadianCourts) {
                if (citation.captured(2) == QLatin1StringView(court.code))
                    details.court = QString::fromLatin1(court.court);
            }
        }
        details.jurisdiction = QStringLiteral("Canada");
        courtHeading = true;
        nameOptional = true;
    } else if (ecliMatch.hasMatch()
               && containsAny(lower, {QStringLiteral("judgment of the court"), QStringLiteral("judgment of the general court"),
                                      QStringLiteral("order of the court")})) {
        details.neutralCitation = ecliMatch.captured(0);
        details.year = ecliMatch.captured(2).toInt();
        details.court = ecliMatch.captured(1) == QLatin1StringView("C") ? QStringLiteral("Court of Justice")
                                                                         : QStringLiteral("General Court");
        details.jurisdiction = QStringLiteral("European Union");
        static const QRegularExpression euCase(QStringLiteral("\\bCases?\\s+([CT])\\s*-\\s*(\\d{1,4}/\\d\\d)\\b"));
        if (const QRegularExpressionMatch match = euCase.match(text); match.hasMatch())
            details.caseNumber = match.captured(1) + u'-' + match.captured(2);
        courtHeading = true;
        nameOptional = true;
    } else {
        // An American court's heading, which names it at the top.
        struct UsCourt {
            const char *heading;
            const char *court; // As the Bluebook abbreviates it.
        };
        static const UsCourt usCourts[] = {
            {"supreme court of the united states", "U.S."},
            {"united states court of appeals for the first circuit", "1st Cir."},
            {"united states court of appeals for the second circuit", "2d Cir."},
            {"united states court of appeals for the third circuit", "3d Cir."},
            {"united states court of appeals for the fourth circuit", "4th Cir."},
            {"united states court of appeals for the fifth circuit", "5th Cir."},
            {"united states court of appeals for the sixth circuit", "6th Cir."},
            {"united states court of appeals for the seventh circuit", "7th Cir."},
            {"united states court of appeals for the eighth circuit", "8th Cir."},
            {"united states court of appeals for the ninth circuit", "9th Cir."},
            {"united states court of appeals for the tenth circuit", "10th Cir."},
            {"united states court of appeals for the eleventh circuit", "11th Cir."},
            {"united states court of appeals for the district of columbia circuit", "D.C. Cir."},
            {"united states court of appeals for the federal circuit", "Fed. Cir."},
            {"united states district court", ""},
        };
        for (const UsCourt &court : usCourts) {
            if (lower.contains(QLatin1StringView(court.heading))) {
                courtHeading = true;
                details.court = QString::fromLatin1(court.court);
                details.jurisdiction = QStringLiteral("United States");
                break;
            }
        }
        if (courtHeading && !containsAny(lower, {QStringLiteral("opinion of the court"), QStringLiteral("petitioner"),
                                                  QStringLiteral("respondent"), QStringLiteral("appellant"),
                                                  QStringLiteral("appellee"), QStringLiteral("plaintiff"),
                                                  QStringLiteral("defendant"), QStringLiteral("argued"),
                                                  QStringLiteral("decided")})) {
            courtHeading = false;
        }
        static const QRegularExpression docket(QStringLiteral("\\bNos?\\.\\s*(\\d{1,2}[-–]\\d{1,6})"));
        if (const QRegularExpressionMatch match = docket.match(text); match.hasMatch())
            details.caseNumber = QStringLiteral("No. ") + match.captured(1).replace(QStringLiteral("–"), QStringLiteral("-"));
        static const QRegularExpression decided(
            QStringLiteral("\\bDecided\\s+(January|February|March|April|May|June|July|August|September|October|November|December)"
                           "\\s+(\\d{1,2}),\\s+((?:19|20)\\d\\d)"));
        if (const QRegularExpressionMatch match = decided.match(text); match.hasMatch()) {
            static const QStringList months = {QStringLiteral("January"), QStringLiteral("February"), QStringLiteral("March"),
                                               QStringLiteral("April"), QStringLiteral("May"), QStringLiteral("June"),
                                               QStringLiteral("July"), QStringLiteral("August"), QStringLiteral("September"),
                                               QStringLiteral("October"), QStringLiteral("November"), QStringLiteral("December")};
            details.month = int(months.indexOf(match.captured(1))) + 1;
            details.day = match.captured(2).toInt();
            details.year = match.captured(3).toInt();
        }
    }
    if (!courtHeading)
        return std::nullopt;

    if (details.caseNumber.isEmpty()) {
        static const QRegularExpression ukCaseNumber(QStringLiteral("Case\\s+No(?:s)?[.:]?\\s*:?\\s*([A-Z0-9][A-Z0-9/.\\-]{3,30}[A-Z0-9])"));
        if (const QRegularExpressionMatch match = ukCaseNumber.match(text); match.hasMatch())
            details.caseNumber = match.captured(1).simplified();
    }

    // The parties, as a line naming them on either side of "v": "Donoghue v Stevenson", "R (Miller) v Secretary of
    // State", "BROWN ET AL. v. BOARD OF EDUCATION".
    static const QRegularExpression parties(
        QStringLiteral("^\\s*(\\p{Lu}[\\p{L}\\d.,'’&()\\- ]{0,120}?)\\s+(?:v|vs|V)\\.?\\s+(\\p{Lu}[\\p{L}\\d.,'’&()\\- ]{0,120}?)\\s*$"),
        QRegularExpression::MultilineOption);
    QRegularExpressionMatch match;
    if (details.name.isEmpty()) {
        match = parties.match(title.simplified());
        if (!match.hasMatch())
            match = parties.match(text);
    }
    if (!details.name.isEmpty()) {
        // Already named, as the Supreme Court Reports index it.
    } else if (match.hasMatch()) {
        const QString parties = match.captured(1).simplified() + u' ' + match.captured(2).simplified();
        details.name = match.captured(1).simplified() + QStringLiteral(" v ") + match.captured(2).simplified();
        // Headings are often all capitals, which reads better as a title.
        if (parties == parties.toUpper()) {
            static const QStringList small = {QStringLiteral("v"), QStringLiteral("of"), QStringLiteral("and"),
                                              QStringLiteral("the"), QStringLiteral("et"), QStringLiteral("al."),
                                              QStringLiteral("for"), QStringLiteral("in")};
            QStringList words = details.name.toLower().split(u' ');
            for (qsizetype i = 0; i < words.size(); ++i) {
                QString &word = words[i];
                if (!word.isEmpty() && (i == 0 || !small.contains(word)))
                    word[0] = word.at(0).toUpper();
            }
            details.name = words.join(u' ');
        }
    } else if (!nameOptional && !containsAny(lower, {QStringLiteral("petitioner"), QStringLiteral("respondent"), QStringLiteral("appellant"),
                                    QStringLiteral("appellee"), QStringLiteral("claimant"), QStringLiteral("plaintiff"),
                                    QStringLiteral("defendant")})) {
        return std::nullopt; // Neither its name nor its parties: not enough to go on.
    }
    return details;
}

Classification classify(const ClassificationInput &input)
{
    // Only the first page decides a type firmly. Importing real papers showed why: their reference
    // lists are full of "PhD thesis", "Technical report" and "Proceedings of".
    const QString text = input.firstPage.toLower();
    const QString later = input.laterPages.toLower();
    const QString title = input.title.toLower();
    const int pages = input.pageCount.value_or(0);
    const bool ebookFormat = QStringList{QStringLiteral("epub"), QStringLiteral("mobi"), QStringLiteral("azw3"),
                                         QStringLiteral("fb2")}.contains(input.format);

    // A real thesis is long, and its wording can be on a title page after a blank first page.
    const QString front = text + u' ' + later.left(4000);
    const bool longEnoughForThesis = pages == 0 || pages >= 30;

    // A judgment says so on its first page, though never so plainly that it's certain.
    if (caseDetailsOnFirstPage(input.firstPage, input.title, input.laterPages))
        return {PublicationType::CaseLaw, false};

    static const QRegularExpression standardNumber(
        QStringLiteral("\\b(iso|iec|astm|ansi|din|bs en|ieee std|en)\\s*[/-]?\\s*[a-z]?\\s*\\d{2,}"));
    if (containsAny(text, {QStringLiteral("international standard"), QStringLiteral("british standard"),
                           QStringLiteral("european standard"), QStringLiteral("american national standard"),
                           QStringLiteral("standard specification for"), QStringLiteral("standard test method")})
        && standardNumber.match(text).hasMatch()) {
        return {PublicationType::Standard, true};
    }

    if (longEnoughForThesis) {
        if (containsAny(front, {QStringLiteral("doctor of philosophy"), QStringLiteral("doctoral dissertation"),
                                QStringLiteral("doctoral thesis"), QStringLiteral("phd dissertation"),
                                QStringLiteral("ph.d. dissertation")})) {
            return {PublicationType::Dissertation, true};
        }
        if (containsAny(front, {QStringLiteral("partial fulfil"), QStringLiteral("master's thesis"),
                                QStringLiteral("masters thesis"), QStringLiteral("master of science"),
                                QStringLiteral("a thesis submitted"), QStringLiteral("phd thesis"),
                                QStringLiteral("ph.d. thesis")})) {
            return {PublicationType::Thesis, true};
        }
    }

    if (containsAny(text, {QStringLiteral("technical report"), QStringLiteral("tech. rep"),
                           QStringLiteral("technical memorandum"), QStringLiteral("research report")})) {
        return {PublicationType::TechnicalReport, true};
    }

    // Some journals have "Proceedings" in their names, so those don't count as conference evidence.
    QString conferenceText = title + u' ' + text;
    for (const char *journal : {"proceedings of the national academy", "proceedings of the ieee",
                                "proceedings of the royal society", "proc. natl. acad", "proc. ieee",
                                "proceedings of the american mathematical society"})
        conferenceText.remove(QString::fromLatin1(journal));

    if (containsAny(conferenceText, {QStringLiteral("proceedings of"), QStringLiteral("in proc."),
                                     QStringLiteral("conference on"), QStringLiteral("symposium on"),
                                     QStringLiteral("workshop on")})) {
        return {pages >= 100 ? PublicationType::ConferenceProceedings : PublicationType::ConferencePaper,
                input.isbn.isEmpty() || pages >= 100};
    }

    if (!input.isbn.isEmpty()) {
        const bool textbook = containsAny(text, {QStringLiteral("textbook"), QStringLiteral(" edition"),
                                                 QStringLiteral("exercises")});
        return {textbook ? PublicationType::Textbook : PublicationType::Book, true};
    }

    // A long file with a journal's ISSN and no ISBN is probably a whole volume.
    if (!input.issn.isEmpty() && pages >= 150)
        return {PublicationType::JournalIssue, false};

    if (!input.doi.isEmpty() && pages > 0 && pages < 80)
        return {PublicationType::JournalArticle, true};

    // A DOI and no ISBN usually means an article, even a very long one such as a systematic review.
    if (!input.doi.isEmpty() || !input.issn.isEmpty())
        return {PublicationType::JournalArticle, false};

    if (pages > 0 && pages < 60
        && containsAny(text + u' ' + later, {QStringLiteral("abstract"), QStringLiteral("references"),
                                             QStringLiteral("vol."), QStringLiteral("journal")})) {
        return {PublicationType::JournalArticle, false};
    }

    if (ebookFormat || pages >= 150)
        return {PublicationType::Book, false};

    return {PublicationType::Other, false};
}

} // namespace papertrail::import
