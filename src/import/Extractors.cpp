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

} // namespace

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

    if (!input.arxivId.isEmpty())
        return {PublicationType::Preprint, true};

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
        return {PublicationType::JournalVolume, false};

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
