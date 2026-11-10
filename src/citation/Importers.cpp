#include "citation/Importers.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

#include <optional>

namespace papertrail::citation {

using namespace core;

namespace {

// ---------------------------------------------------------------------------------------------------- Shared

std::optional<int> monthFrom(const QString &value)
{
    static const QStringList names = {QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"), QStringLiteral("apr"),
                                      QStringLiteral("may"), QStringLiteral("jun"), QStringLiteral("jul"), QStringLiteral("aug"),
                                      QStringLiteral("sep"), QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    const QString text = value.trimmed().toLower();
    bool isNumber = false;
    const int number = text.toInt(&isNumber);
    if (isNumber)
        return number >= 1 && number <= 12 ? std::optional<int>(number) : std::nullopt;
    for (qsizetype i = 0; i < names.size(); ++i) {
        if (text.startsWith(names.at(i)))
            return int(i) + 1;
    }
    return std::nullopt;
}

// A date as "2020", "2020-05-03" or "2020/05/03/", into the year, month and day.
void setDate(Publication &p, const QString &value)
{
    static const QRegularExpression date(QStringLiteral("((?:1[5-9]|20)\\d\\d)(?:[-/](\\d{1,2})?(?:[-/](\\d{1,2}))?)?"));
    const QRegularExpressionMatch match = date.match(value);
    if (!match.hasMatch())
        return;
    p.year = match.captured(1).toInt();
    if (const int month = match.captured(2).toInt(); month >= 1 && month <= 12) {
        p.month = month;
        if (const int day = match.captured(3).toInt(); day >= 1 && day <= 31)
            p.day = day;
    }
}

// An ISBN or ISSN, told apart by how many digits it has.
void setStandardNumber(Publication &p, const QString &value)
{
    for (const QString &part : value.split(QRegularExpression(QStringLiteral("[,;\\s]+(?=\\d)")), Qt::SkipEmptyParts)) {
        QString digits;
        for (const QChar c : part) {
            if (c.isDigit() || c.toUpper() == u'X')
                digits += c.toUpper();
        }
        if (digits.size() == 8 && p.issn.isEmpty())
            p.issn = digits.left(4) + u'-' + digits.mid(4);
        else if (digits.size() == 13 && p.isbn13.isEmpty())
            p.isbn13 = digits;
        else if (digits.size() == 10 && p.isbn10.isEmpty())
            p.isbn10 = digits;
    }
}

// A keyword as a tag path. Papertrail's own exports write nested tags as "A > B", which come back nested; any other
// ">" can't be in a tag's name.
QString tagFrom(const QString &keyword)
{
    QStringList parts;
    for (QString part : keyword.split(QStringLiteral(" > "))) {
        part = part.simplified();
        part.replace(u'>', QStringLiteral("›"));
        if (!part.isEmpty())
            parts << part;
    }
    return parts.join(QStringLiteral(" > "));
}

void addTags(ImportedReference &entry, const QString &keywords, const QString &separators)
{
    for (const QString &keyword : keywords.split(QRegularExpression(separators), Qt::SkipEmptyParts)) {
        const QString tag = tagFrom(keyword);
        if (!tag.isEmpty() && !entry.tags.contains(tag, Qt::CaseInsensitive))
            entry.tags << tag;
    }
}

void addNote(ImportedReference &entry, const QString &note)
{
    const QString text = note.trimmed();
    if (text.isEmpty())
        return;
    entry.note = entry.note.isEmpty() ? text : entry.note + QStringLiteral("\n\n") + text;
}

QString withoutDoiAddress(const QString &value)
{
    static const QRegularExpression prefix(QStringLiteral("^\\s*(?:https?://(?:dx\\.)?doi\\.org/|doi:\\s*)"),
                                           QRegularExpression::CaseInsensitiveOption);
    QString doi = value.trimmed();
    doi.remove(prefix);
    return doi.trimmed();
}

// ---------------------------------------------------------------------------------------------------- LaTeX

QChar combiningAccent(QChar command)
{
    switch (command.unicode()) {
    case '\'': return QChar(0x0301);
    case '`': return QChar(0x0300);
    case '^': return QChar(0x0302);
    case '"': return QChar(0x0308);
    case '~': return QChar(0x0303);
    case '=': return QChar(0x0304);
    case '.': return QChar(0x0307);
    case 'u': return QChar(0x0306);
    case 'v': return QChar(0x030C);
    case 'H': return QChar(0x030B);
    case 'c': return QChar(0x0327);
    case 'k': return QChar(0x0328);
    case 'r': return QChar(0x030A);
    case 'd': return QChar(0x0323);
    case 'b': return QChar(0x0331);
    default: return {};
    }
}

const QHash<QString, QString> &namedCharacters()
{
    static const QHash<QString, QString> characters = {
        {QStringLiteral("ss"), QStringLiteral("ß")},  {QStringLiteral("o"), QStringLiteral("ø")},
        {QStringLiteral("O"), QStringLiteral("Ø")},   {QStringLiteral("ae"), QStringLiteral("æ")},
        {QStringLiteral("AE"), QStringLiteral("Æ")},  {QStringLiteral("oe"), QStringLiteral("œ")},
        {QStringLiteral("OE"), QStringLiteral("Œ")},  {QStringLiteral("l"), QStringLiteral("ł")},
        {QStringLiteral("L"), QStringLiteral("Ł")},   {QStringLiteral("aa"), QStringLiteral("å")},
        {QStringLiteral("AA"), QStringLiteral("Å")},  {QStringLiteral("i"), QStringLiteral("ı")},
        {QStringLiteral("j"), QStringLiteral("ȷ")},   {QStringLiteral("textbackslash"), QStringLiteral("\\")},
        {QStringLiteral("textasciitilde"), QStringLiteral("~")}, {QStringLiteral("textasciicircum"), QStringLiteral("^")},
        {QStringLiteral("textendash"), QStringLiteral("–")}, {QStringLiteral("textemdash"), QStringLiteral("—")},
        {QStringLiteral("textquoteright"), QStringLiteral("’")}, {QStringLiteral("textquoteleft"), QStringLiteral("‘")},
        {QStringLiteral("textquotedblleft"), QStringLiteral("“")}, {QStringLiteral("textquotedblright"), QStringLiteral("”")},
        {QStringLiteral("textregistered"), QStringLiteral("®")}, {QStringLiteral("texttrademark"), QStringLiteral("™")},
        {QStringLiteral("textdegree"), QStringLiteral("°")}, {QStringLiteral("S"), QStringLiteral("§")},
        {QStringLiteral("P"), QStringLiteral("¶")},   {QStringLiteral("dag"), QStringLiteral("†")},
        {QStringLiteral("ldots"), QStringLiteral("…")}, {QStringLiteral("dots"), QStringLiteral("…")},
        {QStringLiteral("alpha"), QStringLiteral("α")}, {QStringLiteral("beta"), QStringLiteral("β")},
        {QStringLiteral("gamma"), QStringLiteral("γ")}, {QStringLiteral("delta"), QStringLiteral("δ")},
        {QStringLiteral("mu"), QStringLiteral("μ")},  {QStringLiteral("pi"), QStringLiteral("π")},
        {QStringLiteral("sigma"), QStringLiteral("σ")}, {QStringLiteral("omega"), QStringLiteral("ω")},
        {QStringLiteral("times"), QStringLiteral("×")}, {QStringLiteral("pm"), QStringLiteral("±")},
        {QStringLiteral("deg"), QStringLiteral("°")},
    };
    return characters;
}

// Splits on a separator outside any braces, as BibTeX reads "and" between names.
QStringList splitOutsideBraces(const QString &text, const QRegularExpression &separator)
{
    QStringList parts;
    int depth = 0;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'{') {
            ++depth;
        } else if (c == u'}') {
            depth = qMax(0, depth - 1);
        } else if (depth == 0) {
            const QRegularExpressionMatch match = separator.match(text, i, QRegularExpression::NormalMatch,
                                                                   QRegularExpression::AnchorAtOffsetMatchOption);
            if (match.hasMatch() && match.capturedLength() > 0) {
                parts << text.mid(start, i - start);
                i += match.capturedLength() - 1;
                start = i + 1;
            }
        }
    }
    parts << text.mid(start);
    return parts;
}

// ---------------------------------------------------------------------------------------------------- BibTeX

struct RawEntry {
    QString type;
    QString key;
    QHash<QString, QString> fields; // By lower-case name, with their braces and quotes taken off.
    int line = 0;
};

class BibTeXReader {
public:
    explicit BibTeXReader(const QString &text) : m_text(text) {}

    QList<RawEntry> read(QStringList &problems)
    {
        QList<RawEntry> entries;
        while (true) {
            const qsizetype at = m_text.indexOf(u'@', m_pos);
            if (at < 0)
                break;
            m_pos = at + 1;
            const int line = lineAt(at);
            const QString type = readName().toLower();
            skipSpace();
            if (type.isEmpty() || m_pos >= m_text.size() || (m_text.at(m_pos) != u'{' && m_text.at(m_pos) != u'('))
                continue; // An @ in a comment between entries.
            const QChar close = m_text.at(m_pos) == u'{' ? u'}' : u')';
            ++m_pos;
            if (type == QLatin1StringView("comment") || type == QLatin1StringView("preamble")) {
                skipBalanced(close);
                continue;
            }
            try {
                if (type == QLatin1StringView("string")) {
                    readStringMacro(close);
                    continue;
                }
                RawEntry entry;
                entry.type = type;
                entry.line = line;
                entry.key = readKey(close);
                readFields(entry, close);
                entries << entry;
            } catch (const QString &problem) {
                problems << QStringLiteral("Line %1: %2").arg(line).arg(problem);
                // Carry on from the next entry.
                const qsizetype next = m_text.indexOf(QRegularExpression(QStringLiteral("\\n\\s*@")), at + 1);
                m_pos = next < 0 ? m_text.size() : next;
            }
        }
        return entries;
    }

private:
    int lineAt(qsizetype position)
    {
        while (m_lineCountedTo < position) {
            if (m_text.at(m_lineCountedTo) == u'\n')
                ++m_line;
            ++m_lineCountedTo;
        }
        return m_line;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    QString readName()
    {
        const qsizetype start = m_pos;
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos);
            if (!(c.isLetterOrNumber() || c == u'_' || c == u'-' || c == u':' || c == u'.' || c == u'+' || c == u'/'))
                break;
            ++m_pos;
        }
        return m_text.mid(start, m_pos - start);
    }

    void skipBalanced(QChar close)
    {
        int depth = 0;
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos++);
            if (c == u'{')
                ++depth;
            else if (c == u'}' && depth > 0)
                --depth;
            else if (c == close && depth == 0)
                return;
        }
    }

    QString readKey(QChar close)
    {
        skipSpace();
        const qsizetype start = m_pos;
        while (m_pos < m_text.size() && m_text.at(m_pos) != u',' && m_text.at(m_pos) != close && m_text.at(m_pos) != u'\n')
            ++m_pos;
        const QString key = m_text.mid(start, m_pos - start).trimmed();
        if (m_pos < m_text.size() && m_text.at(m_pos) == u',')
            ++m_pos;
        return key;
    }

    // "{...}" with braces inside, "..." the same, a number, or a macro, joined by #.
    QString readValue()
    {
        QString value;
        while (true) {
            skipSpace();
            if (m_pos >= m_text.size())
                throw QStringLiteral("the entry isn't closed");
            const QChar c = m_text.at(m_pos);
            if (c == u'{' || c == u'"') {
                const QChar end = c == u'{' ? u'}' : u'"';
                ++m_pos;
                int depth = 0;
                const qsizetype start = m_pos;
                while (true) {
                    if (m_pos >= m_text.size())
                        throw QStringLiteral("a value isn't closed");
                    const QChar d = m_text.at(m_pos);
                    if (d == u'\\') {
                        m_pos += 2;
                        continue;
                    }
                    if (d == u'{') {
                        ++depth;
                    } else if (d == u'}' && depth > 0) {
                        --depth;
                    } else if (d == end && depth == 0) {
                        break;
                    }
                    ++m_pos;
                }
                value += m_text.mid(start, m_pos - start);
                ++m_pos;
            } else {
                const QString name = readName();
                if (name.isEmpty())
                    throw QStringLiteral("a value can't be read");
                bool isNumber = false;
                name.toInt(&isNumber);
                if (isNumber) {
                    value += name;
                } else {
                    const QString macro = name.toLower();
                    if (m_macros.contains(macro))
                        value += m_macros.value(macro);
                    else if (const std::optional<int> month = monthFrom(macro); month && macro.size() == 3)
                        value += QString::number(*month);
                    else
                        value += name;
                }
            }
            skipSpace();
            if (m_pos < m_text.size() && m_text.at(m_pos) == u'#') {
                ++m_pos;
                continue;
            }
            return value;
        }
    }

    void readStringMacro(QChar close)
    {
        skipSpace();
        const QString name = readName().toLower();
        skipSpace();
        if (name.isEmpty() || m_pos >= m_text.size() || m_text.at(m_pos) != u'=')
            throw QStringLiteral("a @string can't be read");
        ++m_pos;
        m_macros.insert(name, readValue());
        skipSpace();
        if (m_pos < m_text.size() && m_text.at(m_pos) == close)
            ++m_pos;
    }

    void readFields(RawEntry &entry, QChar close)
    {
        while (true) {
            skipSpace();
            while (m_pos < m_text.size() && m_text.at(m_pos) == u',') {
                ++m_pos;
                skipSpace();
            }
            if (m_pos >= m_text.size())
                throw QStringLiteral("the entry isn't closed");
            if (m_text.at(m_pos) == close) {
                ++m_pos;
                return;
            }
            const QString name = readName().toLower();
            skipSpace();
            if (name.isEmpty() || m_pos >= m_text.size() || m_text.at(m_pos) != u'=')
                throw QStringLiteral("a field can't be read");
            ++m_pos;
            const QString value = readValue();
            if (!entry.fields.contains(name))
                entry.fields.insert(name, value);
        }
    }

    const QString &m_text;
    qsizetype m_pos = 0;
    qsizetype m_lineCountedTo = 0;
    int m_line = 1;
    QHash<QString, QString> m_macros;
};

std::optional<PublicationType> bibTeXType(const QString &type, bool &preprint)
{
    preprint = false;
    if (type == QLatin1StringView("article"))
        return PublicationType::JournalArticle;
    if (type == QLatin1StringView("book") || type == QLatin1StringView("mvbook") || type == QLatin1StringView("booklet"))
        return PublicationType::Book;
    if (type == QLatin1StringView("inbook") || type == QLatin1StringView("incollection") || type == QLatin1StringView("bookinbook"))
        return PublicationType::BookChapter;
    if (type == QLatin1StringView("inproceedings") || type == QLatin1StringView("conference"))
        return PublicationType::ConferencePaper;
    if (type == QLatin1StringView("proceedings") || type == QLatin1StringView("mvproceedings"))
        return PublicationType::ConferenceProceedings;
    if (type == QLatin1StringView("phdthesis"))
        return PublicationType::Dissertation;
    if (type == QLatin1StringView("mastersthesis") || type == QLatin1StringView("thesis"))
        return PublicationType::Thesis;
    if (type == QLatin1StringView("techreport") || type == QLatin1StringView("report"))
        return PublicationType::TechnicalReport;
    if (type == QLatin1StringView("standard"))
        return PublicationType::Standard;
    if (type == QLatin1StringView("jurisdiction") || type == QLatin1StringView("case") || type == QLatin1StringView("legal"))
        return PublicationType::CaseLaw;
    if (type == QLatin1StringView("periodical"))
        return PublicationType::JournalIssue;
    if (type == QLatin1StringView("unpublished") || type == QLatin1StringView("online") || type == QLatin1StringView("misc")
        || type == QLatin1StringView("electronic") || type == QLatin1StringView("www") || type == QLatin1StringView("manual")) {
        return PublicationType::Other;
    }
    return std::nullopt;
}

ImportedReference fromBibTeX(const RawEntry &raw)
{
    ImportedReference entry;
    entry.key = raw.key;
    entry.line = raw.line;
    const auto text = [&](const char *name) { return decodeLatex(raw.fields.value(QString::fromLatin1(name))); };
    // Identifiers, addresses and paths aren't LaTeX, though they're braced.
    const auto exact = [&](const char *name) {
        QString value = raw.fields.value(QString::fromLatin1(name));
        value.remove(u'{').remove(u'}');
        return value.trimmed();
    };
    Publication &p = entry.publication;

    bool preprint = false;
    p.type = bibTeXType(raw.type, preprint).value_or(PublicationType::Other);
    const QString degree = text("type");
    if (p.type == PublicationType::Thesis && (degree.contains(QStringLiteral("phd"), Qt::CaseInsensitive)
                                              || degree.contains(QStringLiteral("doctor"), Qt::CaseInsensitive))) {
        p.type = PublicationType::Dissertation;
    }

    p.title = text("title");
    p.subtitle = text("subtitle");
    p.authors = parseBibTeXNames(raw.fields.value(QStringLiteral("author")));
    p.editors = parseBibTeXNames(raw.fields.value(QStringLiteral("editor")));
    p.translators = parseBibTeXNames(raw.fields.value(QStringLiteral("translator")));

    QString container = text("journaltitle");
    if (container.isEmpty())
        container = text("journal");
    if (container.isEmpty())
        container = text("booktitle");
    p.containerTitle = container;

    if (!exact("date").isEmpty())
        setDate(p, exact("date"));
    else if (!exact("year").isEmpty())
        setDate(p, exact("year"));
    if (p.year && !p.month) {
        if (const std::optional<int> month = monthFrom(exact("month")))
            p.month = month;
    }

    p.volume = text("volume");
    const QString number = text("number");
    if (p.type == PublicationType::TechnicalReport)
        p.reportNumber = number;
    else if (p.type == PublicationType::Standard)
        p.standardNumber = number;
    else if (p.type == PublicationType::CaseLaw)
        p.caseNumber = number;
    else
        p.issue = number.isEmpty() ? text("issue") : number;
    QString pages = exact("pages");
    pages.replace(QRegularExpression(QStringLiteral("\\s*-{1,3}\\s*|\\s*–\\s*")), QStringLiteral("-"));
    p.pages = pages;
    p.edition = text("edition");
    p.series = text("series");

    p.doi = withoutDoiAddress(exact("doi"));
    p.url = exact("url");
    if (p.doi.isEmpty() && p.url.contains(QStringLiteral("doi.org/"), Qt::CaseInsensitive))
        p.doi = withoutDoiAddress(p.url);
    setStandardNumber(p, exact("isbn"));
    setStandardNumber(p, exact("issn"));
    p.pmid = exact("pmid");
    const QString eprintType = (exact("archiveprefix") + exact("eprinttype")).toLower();
    if (eprintType.contains(QStringLiteral("arxiv")) && !exact("eprint").isEmpty())
        p.arxivId = exact("eprint");
    // Zotero keeps an arXiv number in the note, the DOI arXiv gives it, or its address.
    if (p.arxivId.isEmpty()) {
        static const QRegularExpression arxiv(
            QStringLiteral("(?:arXiv:|10\\.48550/arXiv\\.|arxiv\\.org/abs/)(\\d{4}\\.\\d{4,5}(?:v\\d+)?)"), QRegularExpression::CaseInsensitiveOption);
        for (const QString &where : {text("note"), p.doi, p.url}) {
            if (const QRegularExpressionMatch match = arxiv.match(where); match.hasMatch()) {
                p.arxivId = match.captured(1);
                break;
            }
        }
    }
    p.abstract = text("abstract");
    p.language = text("language");

    p.publisher = text("publisher");
    p.place = text("location").isEmpty() ? text("address") : text("location");
    const QString institution = text("school").isEmpty() ? text("institution") : text("school");
    if (p.type == PublicationType::CaseLaw)
        p.court = institution.isEmpty() ? text("court") : institution;
    else
        p.institution = institution;
    if (p.type == PublicationType::Thesis || p.type == PublicationType::Dissertation)
        p.degree = degree;
    if (p.type == PublicationType::Standard)
        p.standardBody = text("organization");
    else if (p.publisher.isEmpty())
        p.publisher = text("organization");
    p.conferenceName = text("eventtitle");
    p.conferenceLocation = text("venue");

    // Zotero and others keep a preprint as a misc entry from arXiv, or an article "in" arXiv.
    const QString howPublished = text("howpublished");
    entry.preprintHint = !p.arxivId.isEmpty() && (p.type == PublicationType::Other || container.contains(QStringLiteral("arxiv"), Qt::CaseInsensitive));
    entry.preprintHint = entry.preprintHint || raw.type == QLatin1StringView("unpublished")
                         || howPublished.contains(QStringLiteral("preprint"), Qt::CaseInsensitive)
                         || (!p.arxivId.isEmpty() && p.publisher.compare(QStringLiteral("arxiv"), Qt::CaseInsensitive) == 0);
    // A note that only gives the arXiv number says nothing of the user's own.
    static const QRegularExpression arxivNote(QStringLiteral("^arXiv:\\S+(?:\\s*\\[[^\\]]*\\])?$"), QRegularExpression::CaseInsensitiveOption);
    const bool noteIsArxiv = arxivNote.match(text("note")).hasMatch();
    if (container.compare(QStringLiteral("arxiv"), Qt::CaseInsensitive) == 0 || container.startsWith(QStringLiteral("arXiv"))) {
        p.containerTitle.clear();
        p.type = PublicationType::Other;
    }

    addTags(entry, text("keywords"), QStringLiteral("[,;]"));
    addTags(entry, text("mendeley-tags"), QStringLiteral("[,;]"));
    addTags(entry, text("groups"), QStringLiteral(","));
    addNote(entry, text("annote"));
    addNote(entry, text("annotation"));
    if (!noteIsArxiv)
        addNote(entry, text("note"));
    entry.files = parseFileField(raw.fields.value(QStringLiteral("file")));
    return entry;
}

// ---------------------------------------------------------------------------------------------------- RIS

PublicationType risType(const QString &type, bool &preprint)
{
    preprint = false;
    static const QHash<QString, PublicationType> types = {
        {QStringLiteral("JOUR"), PublicationType::JournalArticle}, {QStringLiteral("EJOUR"), PublicationType::JournalArticle},
        {QStringLiteral("MGZN"), PublicationType::JournalArticle}, {QStringLiteral("INPR"), PublicationType::JournalArticle},
        {QStringLiteral("JFULL"), PublicationType::JournalIssue},  {QStringLiteral("BOOK"), PublicationType::Book},
        {QStringLiteral("EBOOK"), PublicationType::Book},          {QStringLiteral("EDBOOK"), PublicationType::Book},
        {QStringLiteral("CHAP"), PublicationType::BookChapter},    {QStringLiteral("ECHAP"), PublicationType::BookChapter},
        {QStringLiteral("CPAPER"), PublicationType::ConferencePaper}, {QStringLiteral("CONF"), PublicationType::ConferenceProceedings},
        {QStringLiteral("THES"), PublicationType::Thesis},         {QStringLiteral("RPRT"), PublicationType::TechnicalReport},
        {QStringLiteral("STAND"), PublicationType::Standard},      {QStringLiteral("CASE"), PublicationType::CaseLaw},
    };
    if (type == QLatin1StringView("UNPB"))
        preprint = true;
    return types.value(type, PublicationType::Other);
}

ImportedReference fromRis(const QList<QPair<QString, QString>> &lines, int line)
{
    ImportedReference entry;
    entry.line = line;
    Publication &p = entry.publication;
    QHash<QString, QStringList> values;
    for (const auto &[tag, value] : lines)
        values[tag] << value.trimmed();
    const auto first = [&](std::initializer_list<const char *> tags) {
        for (const char *tag : tags) {
            for (const QString &value : values.value(QString::fromLatin1(tag))) {
                if (!value.isEmpty())
                    return value;
            }
        }
        return QString();
    };
    const auto people = [&](std::initializer_list<const char *> tags) {
        QList<PersonName> found;
        for (const char *tag : tags) {
            for (const QString &value : values.value(QString::fromLatin1(tag))) {
                if (value.isEmpty())
                    continue;
                PersonName person;
                person.family = value.section(u',', 0, 0).trimmed();
                person.given = value.section(u',', 1).trimmed();
                found << person;
            }
        }
        return found;
    };

    bool preprint = false;
    const QString type = first({"TY"});
    p.type = risType(type, preprint);
    entry.preprintHint = preprint;
    entry.key = first({"ID"});

    p.title = first({"TI", "T1", "CT"});
    p.authors = people({"AU", "A1"});
    const bool secondIsBody = p.type == PublicationType::Standard;
    p.editors = secondIsBody ? people({"ED"}) : people({"ED", "A2"});
    p.containerTitle = first({"T2", "JF", "JO", "BT", "JA", "J2"});
    if (p.type == PublicationType::Book && p.containerTitle == p.title)
        p.containerTitle.clear();
    p.series = first({"T3"});

    const QString date = first({"DA", "PY", "Y1"});
    setDate(p, date);
    if (!p.year)
        setDate(p, first({"PY", "Y1"}));

    const QString volume = first({"VL"});
    if (p.type == PublicationType::TechnicalReport)
        p.reportNumber = volume;
    else if (p.type == PublicationType::Standard)
        p.standardNumber = volume;
    else
        p.volume = volume;
    p.issue = first({"IS"});
    const QString start = first({"SP"});
    const QString end = first({"EP"});
    p.pages = end.isEmpty() || start.contains(u'-') ? start : start + u'-' + end;
    p.edition = first({"ET"});

    const QString publisher = first({"PB"});
    if (p.type == PublicationType::Thesis || p.type == PublicationType::TechnicalReport)
        p.institution = publisher;
    else if (p.type == PublicationType::CaseLaw)
        p.court = publisher;
    else
        p.publisher = publisher;
    if (p.type == PublicationType::Standard)
        p.standardBody = first({"A2"});
    p.place = first({"CY"});
    p.conferenceName = first({"C3"});
    const QString degree = first({"M3"});
    if (p.type == PublicationType::Thesis) {
        p.degree = degree;
        if (degree.contains(QStringLiteral("phd"), Qt::CaseInsensitive) || degree.contains(QStringLiteral("doctor"), Qt::CaseInsensitive))
            p.type = PublicationType::Dissertation;
    }

    p.doi = withoutDoiAddress(first({"DO"}));
    for (const QString &url : values.value(QStringLiteral("UR"))) {
        if (url.contains(QStringLiteral("doi.org/"), Qt::CaseInsensitive) && p.doi.isEmpty())
            p.doi = withoutDoiAddress(url);
        else if (p.url.isEmpty())
            p.url = url;
    }
    for (const QString &number : values.value(QStringLiteral("SN")))
        setStandardNumber(p, number);
    const QString misc = first({"M1"});
    if (misc.startsWith(QStringLiteral("arXiv:"), Qt::CaseInsensitive))
        p.arxivId = misc.mid(6).trimmed();
    else if (p.type == PublicationType::CaseLaw)
        p.caseNumber = misc;
    p.language = first({"LA"});
    p.abstract = first({"AB", "N2"});

    for (const QString &keyword : values.value(QStringLiteral("KW")))
        addTags(entry, keyword, QStringLiteral(";"));
    for (const QString &note : values.value(QStringLiteral("N1"))) {
        static const QRegularExpression neutral(QStringLiteral("^Neutral citation:\\s*(.+)$"), QRegularExpression::CaseInsensitiveOption);
        if (const QRegularExpressionMatch match = neutral.match(note); match.hasMatch() && p.type == PublicationType::CaseLaw)
            p.neutralCitation = match.captured(1).trimmed();
        else
            addNote(entry, note);
    }
    for (const char *tag : {"L1", "L2", "L4"}) {
        for (const QString &link : values.value(QString::fromLatin1(tag))) {
            const QUrl url(link);
            if (url.isLocalFile())
                entry.files << url.toLocalFile();
            else if (url.scheme().isEmpty() || (url.scheme().size() == 1 && link.size() > 2)) // A path, or a drive letter.
                entry.files << link;
        }
    }
    return entry;
}

} // namespace

QString decodeLatex(const QString &input)
{
    QString out;
    out.reserve(input.size());
    const qsizetype size = input.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = input.at(i);
        if (c == u'\\' && i + 1 < size) {
            const QChar next = input.at(i + 1);
            // An escaped character.
            if (QStringLiteral("&%$#_{}").contains(next)) {
                out += next;
                ++i;
                continue;
            }
            if (next == u'\\') { // A line break.
                out += u' ';
                ++i;
                continue;
            }
            // An accent, on the letter after it, in braces or not: \"u, \"{u}, \v{c}, \'\i.
            if (const QChar accent = combiningAccent(next); !accent.isNull() && (!next.isLetter() || i + 2 >= size
                                                                                   || !input.at(i + 2).isLetter()
                                                                                   || QStringLiteral("uvHckrdb").contains(next))) {
                qsizetype j = i + 2;
                if (next.isLetter()) {
                    while (j < size && input.at(j) == u' ')
                        ++j;
                }
                bool braced = false;
                if (j < size && input.at(j) == u'{') {
                    braced = true;
                    ++j;
                }
                QString letter;
                if (j < size && input.at(j) == u'\\' && j + 1 < size && (input.at(j + 1) == u'i' || input.at(j + 1) == u'j')) {
                    letter = input.at(j + 1);
                    j += 2;
                } else if (j < size) {
                    letter = input.at(j);
                    ++j;
                }
                if (braced && j < size && input.at(j) == u'}')
                    ++j;
                if (!letter.isEmpty() && letter.at(0).isLetter()) {
                    out += (letter + accent).normalized(QString::NormalizationForm_C);
                    i = j - 1;
                    continue;
                }
            }
            // A command by name: a character, or formatting whose argument is kept.
            qsizetype j = i + 1;
            while (j < size && input.at(j).isLetter())
                ++j;
            const QString name = input.mid(i + 1, j - i - 1);
            if (!name.isEmpty()) {
                const auto named = namedCharacters().constFind(name);
                if (named != namedCharacters().cend()) {
                    out += *named;
                    // "\o{}" or "\ss " end with braces or a space, which aren't part of the text.
                    if (j + 1 < size && input.at(j) == u'{' && input.at(j + 1) == u'}')
                        j += 2;
                    else if (j < size && input.at(j) == u' ')
                        ++j;
                }
                i = j - 1;
                continue;
            }
            continue; // A stray backslash.
        }
        if (c == u'{' || c == u'}' || c == u'$')
            continue;
        if (c == u'~') {
            out += u' ';
            continue;
        }
        if (c == u'-' && i + 1 < size && input.at(i + 1) == u'-') {
            const bool em = i + 2 < size && input.at(i + 2) == u'-';
            out += em ? QStringLiteral("—") : QStringLiteral("–");
            i += em ? 2 : 1;
            continue;
        }
        if ((c == u'`' || c == u'\'') && i + 1 < size && input.at(i + 1) == c) {
            out += c == u'`' ? QStringLiteral("“") : QStringLiteral("”");
            ++i;
            continue;
        }
        out += c;
    }
    return out.simplified();
}

QList<PersonName> parseBibTeXNames(const QString &names)
{
    static const QRegularExpression andWord(QStringLiteral("\\s+and\\s+"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression comma(QStringLiteral(","));
    static const QRegularExpression space(QStringLiteral("\\s+"));
    QList<PersonName> people;
    for (QString name : splitOutsideBraces(names.simplified(), andWord)) {
        name = name.trimmed();
        if (name.isEmpty() || name.compare(QStringLiteral("others"), Qt::CaseInsensitive) == 0)
            continue;
        PersonName person;
        // A whole name in braces is an organisation's, kept as it is.
        if (name.startsWith(u'{') && name.endsWith(u'}') && splitOutsideBraces(name.mid(1, name.size() - 2), andWord).size() == 1
            && name.count(u'{') == 1) {
            person.family = decodeLatex(name);
            people << person;
            continue;
        }
        const QStringList parts = splitOutsideBraces(name, comma);
        if (parts.size() >= 3) {
            person.family = decodeLatex(parts.at(0)) + u' ' + decodeLatex(parts.at(1)); // "Last, Jr, First"
            person.given = decodeLatex(parts.mid(2).join(u' '));
        } else if (parts.size() == 2) {
            person.family = decodeLatex(parts.at(0));
            person.given = decodeLatex(parts.at(1));
        } else {
            // "Given von Family": the family name is the last word, with any small words just before it.
            QStringList words = splitOutsideBraces(name, space);
            words.removeAll(QString());
            if (words.size() == 1) {
                person.family = decodeLatex(words.first());
            } else {
                qsizetype familyStart = words.size() - 1;
                while (familyStart > 1 && !words.at(familyStart - 1).isEmpty() && words.at(familyStart - 1).at(0).isLower())
                    --familyStart;
                person.family = decodeLatex(words.mid(familyStart).join(u' '));
                person.given = decodeLatex(words.mid(0, familyStart).join(u' '));
            }
        }
        if (!person.family.isEmpty())
            people << person;
    }
    return people;
}

QStringList parseFileField(const QString &field)
{
    QStringList paths;
    QString value = field;
    value.replace(QStringLiteral("$\\backslash$"), QStringLiteral("\\"));
    // Entries split at semicolons, and each into description, path and type at colons, but not escaped ones.
    QStringList entries;
    QString current;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == u'\\' && i + 1 < value.size()) {
            current += c;
            current += value.at(++i);
        } else if (c == u';') {
            entries << current;
            current.clear();
        } else {
            current += c;
        }
    }
    entries << current;
    for (const QString &entry : std::as_const(entries)) {
        QStringList parts;
        QString part;
        for (qsizetype i = 0; i < entry.size(); ++i) {
            const QChar c = entry.at(i);
            if (c == u'\\' && i + 1 < entry.size()) {
                part += entry.at(++i); // "\:" is a colon, and "\\" a backslash.
            } else if (c == u':') {
                parts << part;
                part.clear();
            } else {
                part += c;
            }
        }
        parts << part;
        // A drive letter's colon that wasn't escaped, as in "Description:C:/Users/x.pdf:application/pdf".
        if (parts.size() >= 4 && parts.at(1).size() == 1 && parts.at(1).at(0).isLetter())
            parts = QStringList{parts.at(0), parts.at(1) + u':' + parts.at(2)} + parts.mid(3);
        QString path = parts.size() >= 2 ? parts.at(1) : parts.value(0);
        path = path.trimmed();
        path.remove(u'{').remove(u'}');
        if (!path.isEmpty())
            paths << path;
    }
    return paths;
}

ParsedReferences parseBibTeX(const QString &text)
{
    ParsedReferences parsed;
    parsed.format = ReferenceFormat::BibTeX;
    BibTeXReader reader(text);
    for (const RawEntry &raw : reader.read(parsed.problems)) {
        ImportedReference entry = fromBibTeX(raw);
        if (entry.publication.title.isEmpty() && entry.publication.authors.isEmpty()) {
            parsed.problems << QStringLiteral("Line %1: the entry has no title").arg(raw.line);
            continue;
        }
        parsed.entries << entry;
    }
    return parsed;
}

ParsedReferences parseRis(const QString &text)
{
    ParsedReferences parsed;
    parsed.format = ReferenceFormat::Ris;
    static const QRegularExpression tagLine(QStringLiteral("^([A-Z][A-Z0-9])  -(?: (.*))?$"));
    QList<QPair<QString, QString>> lines;
    int start = 0;
    bool open = false;
    const QStringList all = text.split(u'\n');
    for (qsizetype i = 0; i < all.size(); ++i) {
        QString line = all.at(i);
        if (line.endsWith(u'\r'))
            line.chop(1);
        const QRegularExpressionMatch match = tagLine.match(line);
        if (!match.hasMatch()) {
            // A value carried onto the next line.
            if (open && !lines.isEmpty() && !line.trimmed().isEmpty())
                lines.last().second += u' ' + line.trimmed();
            continue;
        }
        const QString tag = match.captured(1);
        if (tag == QLatin1StringView("TY")) {
            if (open)
                parsed.problems << QStringLiteral("Line %1: the entry before it has no ER line").arg(i + 1);
            lines.clear();
            open = true;
            start = int(i) + 1;
        }
        if (!open)
            continue;
        if (tag == QLatin1StringView("ER")) {
            ImportedReference entry = fromRis(lines, start);
            if (entry.publication.title.isEmpty() && entry.publication.authors.isEmpty())
                parsed.problems << QStringLiteral("Line %1: the entry has no title").arg(start);
            else
                parsed.entries << entry;
            open = false;
            lines.clear();
            continue;
        }
        lines << qMakePair(tag, match.captured(2));
    }
    if (open)
        parsed.problems << QStringLiteral("Line %1: the entry has no ER line").arg(start);
    return parsed;
}

ParsedReferences parseReferences(const QByteArray &bytes)
{
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    static const QRegularExpression risStart(QStringLiteral("^TY  - "), QRegularExpression::MultilineOption);
    static const QRegularExpression bibStart(QStringLiteral("@\\s*[A-Za-z]+\\s*[{(]"));
    const qsizetype ris = text.indexOf(risStart);
    const qsizetype bib = text.indexOf(bibStart);
    if (ris >= 0 && (bib < 0 || ris < bib))
        return parseRis(text);
    if (bib >= 0)
        return parseBibTeX(text);
    ParsedReferences nothing;
    nothing.problems << QStringLiteral("This isn't a BibTeX or RIS file.");
    return nothing;
}

} // namespace papertrail::citation
