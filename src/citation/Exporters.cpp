#include "citation/Exporters.h"

#include "citation/Formatting.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace papertrail::citation {

using core::PersonName;
using Kind = CitationItem::Kind;

QString exportFormatName(ExportFormat format)
{
    switch (format) {
    case ExportFormat::BibTeX: return QStringLiteral("BibTeX");
    case ExportFormat::Ris: return QStringLiteral("RIS");
    case ExportFormat::CslJson: return QStringLiteral("CSL-JSON");
    }
    return {};
}

QString exportFileSuffix(ExportFormat format)
{
    switch (format) {
    case ExportFormat::BibTeX: return QStringLiteral("bib");
    case ExportFormat::Ris: return QStringLiteral("ris");
    case ExportFormat::CslJson: return QStringLiteral("json");
    }
    return {};
}

QString exportFileFilter(ExportFormat format)
{
    return QStringLiteral("%1 (*.%2)").arg(exportFormatName(format), exportFileSuffix(format));
}

namespace {

// Plain lowercase letters and digits, for keys.
QString asciiWord(const QString &text)
{
    QString out;
    for (const QChar c : text.normalized(QString::NormalizationForm_KD)) {
        if (c.unicode() < 128 && c.isLetterOrNumber())
            out += c.toLower();
    }
    return out;
}

QString keyBase(const CitationItem &item)
{
    QString who;
    if (!item.authors.isEmpty())
        who = asciiWord(item.authors.first().family);
    else if (!item.editors.isEmpty())
        who = asciiWord(item.editors.first().family);
    else if (!item.standardBody.isEmpty())
        who = asciiWord(item.standardBody.section(u' ', 0, 0));
    if (who.isEmpty())
        who = QStringLiteral("anon");

    static const QSet<QString> skipped = {QStringLiteral("a"),  QStringLiteral("an"),  QStringLiteral("the"),
                                          QStringLiteral("of"), QStringLiteral("on"),  QStringLiteral("in"),
                                          QStringLiteral("and"), QStringLiteral("for"), QStringLiteral("to"),
                                          QStringLiteral("with"), QStringLiteral("from"), QStringLiteral("at")};
    QString word;
    static const QRegularExpression separators(QStringLiteral("[^\\w]+"), QRegularExpression::UseUnicodePropertiesOption);
    for (const QString &candidate : item.title.split(separators, Qt::SkipEmptyParts)) {
        const QString plain = asciiWord(candidate);
        if (!plain.isEmpty() && !skipped.contains(plain)) {
            word = plain;
            break;
        }
    }
    return who + (item.year ? QString::number(*item.year) : QString()) + word.left(20);
}

QString monthMacro(int month)
{
    static const QStringList months = {QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"), QStringLiteral("apr"),
                                       QStringLiteral("may"), QStringLiteral("jun"), QStringLiteral("jul"), QStringLiteral("aug"),
                                       QStringLiteral("sep"), QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    return month >= 1 && month <= 12 ? months.at(month - 1) : QString();
}

// "Family, Given and Family, Given", with organisations braced so their words aren't taken as names.
QString bibTeXNames(const QList<PersonName> &people)
{
    QStringList names;
    for (const PersonName &person : people) {
        if (isOrganisation(person))
            names << u'{' + bibTeXText(person.family.trimmed()) + u'}';
        else
            names << bibTeXText(person.family.trimmed()) + QStringLiteral(", ") + bibTeXText(person.given.trimmed());
    }
    return names.join(QStringLiteral(" and "));
}

bool isDoctoral(const CitationItem &item)
{
    const DegreeLevel level = degreeLevel(item.degree, item.type);
    return level == DegreeLevel::Doctoral || (level == DegreeLevel::Unknown && item.type == core::PublicationType::Dissertation);
}

} // namespace

QString bibTeXText(const QString &text, bool protectCapitals)
{
    QString escaped;
    escaped.reserve(text.size() + 8);
    for (const QChar c : text.simplified()) {
        switch (c.unicode()) {
        case '\\': escaped += QStringLiteral("\\textbackslash{}"); break;
        case '{': escaped += QStringLiteral("\\{"); break;
        case '}': escaped += QStringLiteral("\\}"); break;
        case '&': escaped += QStringLiteral("\\&"); break;
        case '%': escaped += QStringLiteral("\\%"); break;
        case '$': escaped += QStringLiteral("\\$"); break;
        case '#': escaped += QStringLiteral("\\#"); break;
        case '_': escaped += QStringLiteral("\\_"); break;
        case '~': escaped += QStringLiteral("\\textasciitilde{}"); break;
        case '^': escaped += QStringLiteral("\\textasciicircum{}"); break;
        default: escaped += c;
        }
    }
    if (!protectCapitals)
        return escaped;

    // A word with a capital after its first letter, such as "DNA", "iPhone" or "3D", keeps its case.
    QStringList words = escaped.split(u' ');
    for (QString &word : words) {
        if (word.contains(u'\\'))
            continue; // Already has commands in it, which mustn't be broken up.
        bool laterCapital = false;
        for (qsizetype i = 1; i < word.size(); ++i) {
            if (word.at(i).isUpper()) {
                laterCapital = true;
                break;
            }
        }
        const bool digitsAndCapitals = word.size() > 1 && word.at(0).isDigit()
            && std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isUpper(); });
        if (laterCapital || digitsAndCapitals) {
            // Punctuation at either end stays outside the braces.
            qsizetype start = 0;
            qsizetype end = word.size();
            while (start < end && !word.at(start).isLetterOrNumber())
                ++start;
            while (end > start && !word.at(end - 1).isLetterOrNumber())
                --end;
            if (end > start)
                word = word.left(start) + u'{' + word.mid(start, end - start) + u'}' + word.mid(end);
        }
    }
    return words.join(u' ');
}

QStringList citationKeys(const QList<CitationItem> &items)
{
    QStringList keys;
    QHash<QString, int> used;
    QList<QString> bases;
    for (const CitationItem &item : items)
        bases << keyBase(item);
    QHash<QString, int> counts;
    for (const QString &base : std::as_const(bases))
        ++counts[base];
    for (const QString &base : std::as_const(bases)) {
        if (counts.value(base) == 1) {
            keys << base;
            continue;
        }
        // Repeated keys get letters, as works by one author in one year do.
        const int n = used[base]++;
        QString suffix;
        int value = n;
        do {
            suffix.prepend(QChar(u'a' + value % 26));
            value = value / 26 - 1;
        } while (value >= 0);
        keys << base + suffix;
    }
    return keys;
}

QByteArray toBibTeX(const QList<CitationItem> &items)
{
    const QStringList keys = citationKeys(items);
    QString out;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const CitationItem &item = items.at(i);
        QString type = QStringLiteral("misc");
        QList<std::pair<QString, QString>> fields;
        const auto field = [&](const QString &name, const QString &value, bool raw = false, bool protect = false) {
            if (!value.trimmed().isEmpty())
                fields << std::make_pair(name, raw ? u'{' + value.trimmed() + u'}' : u'{' + bibTeXText(value, protect) + u'}');
        };

        switch (item.kind) {
        case Kind::Article: type = QStringLiteral("article"); break;
        case Kind::Book:
        case Kind::EditedBook: type = QStringLiteral("book"); break;
        case Kind::Chapter: type = QStringLiteral("incollection"); break;
        case Kind::Paper: type = QStringLiteral("inproceedings"); break;
        case Kind::Proceedings: type = QStringLiteral("proceedings"); break;
        case Kind::Thesis:
            type = degreeLevel(item.degree, item.type) == DegreeLevel::Masters ? QStringLiteral("mastersthesis")
                                                                                 : QStringLiteral("phdthesis");
            break;
        case Kind::Report: type = QStringLiteral("techreport"); break;
        case Kind::Issue:
        case Kind::Standard:
        case Kind::Preprint:
        case Kind::Document: type = QStringLiteral("misc"); break;
        }

        if (!item.authors.isEmpty())
            fields << std::make_pair(QStringLiteral("author"), u'{' + bibTeXNames(item.authors) + u'}');
        if (!item.editors.isEmpty() && item.kind != Kind::Article)
            fields << std::make_pair(QStringLiteral("editor"), u'{' + bibTeXNames(item.editors) + u'}');
        // With nobody named, BibTeX sorts by this instead: the organisation responsible, or the title.
        if (item.authors.isEmpty() && (item.editors.isEmpty() || item.kind == Kind::Article)) {
            QString sortBy = item.standardBody;
            if (sortBy.isEmpty())
                sortBy = item.institution.isEmpty() ? item.publisher : item.institution;
            if (sortBy.isEmpty() || item.kind == Kind::Article || item.kind == Kind::Issue)
                sortBy = item.title.isEmpty() ? item.containerTitle : item.title;
            field(QStringLiteral("key"), sortBy);
        }
        field(QStringLiteral("title"), item.kind == Kind::Issue && item.title.isEmpty() ? item.containerTitle : item.title, false, true);

        switch (item.kind) {
        case Kind::Article: field(QStringLiteral("journal"), item.containerTitle); break;
        case Kind::Chapter:
        case Kind::Paper: field(QStringLiteral("booktitle"), item.containerTitle, false, true); break;
        case Kind::Issue:
            if (item.containerTitle != item.title)
                field(QStringLiteral("journal"), item.containerTitle);
            break;
        default: break;
        }

        if (item.year)
            fields << std::make_pair(QStringLiteral("year"), u'{' + QString::number(*item.year) + u'}');
        if (item.month)
            fields << std::make_pair(QStringLiteral("month"), monthMacro(*item.month));
        field(QStringLiteral("volume"), item.volume);
        if (item.kind == Kind::Report)
            field(QStringLiteral("number"), item.reportNumber);
        else if (item.kind == Kind::Standard)
            field(QStringLiteral("number"), item.standardNumber);
        else
            field(QStringLiteral("number"), item.issue);
        if (!item.pages.isEmpty())
            field(QStringLiteral("pages"), joinedPages(item.pages, QStringLiteral("--")));
        field(QStringLiteral("edition"), item.edition);
        field(QStringLiteral("series"), item.series);

        if (item.kind == Kind::Thesis) {
            field(QStringLiteral("school"), item.institution.isEmpty() ? item.publisher : item.institution);
            if (!isDoctoral(item) && degreeLevel(item.degree, item.type) != DegreeLevel::Masters)
                field(QStringLiteral("type"), item.degree.isEmpty() ? QStringLiteral("Thesis") : item.degree);
        } else if (item.kind == Kind::Report) {
            field(QStringLiteral("institution"), item.institution.isEmpty() ? item.publisher : item.institution);
            if (!item.institution.isEmpty())
                field(QStringLiteral("publisher"), item.publisher);
        } else if (item.kind == Kind::Standard) {
            field(QStringLiteral("organization"), item.standardBody);
            field(QStringLiteral("publisher"), item.publisher);
            field(QStringLiteral("type"), QStringLiteral("Standard"));
        } else {
            field(QStringLiteral("publisher"), item.publisher);
        }
        field(QStringLiteral("address"), item.place);
        if (item.kind == Kind::Paper || item.kind == Kind::Proceedings) {
            field(QStringLiteral("eventtitle"), item.conferenceName);
            field(QStringLiteral("venue"), item.conferenceLocation);
        }
        if (!item.arxivId.isEmpty()) {
            field(QStringLiteral("eprint"), item.arxivId, true);
            field(QStringLiteral("archiveprefix"), QStringLiteral("arXiv"), true);
            field(QStringLiteral("eprinttype"), QStringLiteral("arxiv"), true);
        }
        if (item.kind == Kind::Preprint)
            field(QStringLiteral("howpublished"), item.arxivId.isEmpty() ? QStringLiteral("Preprint") : QStringLiteral("arXiv:") + item.arxivId);
        field(QStringLiteral("doi"), item.doi, true);
        field(QStringLiteral("url"), item.url, true);
        field(QStringLiteral("isbn"), item.isbn, true);
        field(QStringLiteral("issn"), item.issn, true);
        field(QStringLiteral("pmid"), item.pmid, true);
        field(QStringLiteral("language"), item.language);
        field(QStringLiteral("keywords"), item.keywords.join(QStringLiteral(", ")));
        field(QStringLiteral("abstract"), item.abstract);

        out += u'@' + type + u'{' + keys.at(i) + QStringLiteral(",\n");
        for (qsizetype f = 0; f < fields.size(); ++f) {
            out += QStringLiteral("  ") + fields.at(f).first + QStringLiteral(" = ") + fields.at(f).second;
            out += f + 1 < fields.size() ? QStringLiteral(",\n") : QStringLiteral("\n");
        }
        out += QStringLiteral("}\n\n");
    }
    return out.toUtf8();
}

QByteArray toRis(const QList<CitationItem> &items)
{
    QString out;
    const auto line = [&](const QString &tag, const QString &value) {
        const QString text = value.simplified();
        if (!text.isEmpty())
            out += tag + QStringLiteral("  - ") + text + QStringLiteral("\r\n");
    };
    const auto name = [](const PersonName &person) {
        return isOrganisation(person) ? person.family.trimmed() : person.family.trimmed() + QStringLiteral(", ") + person.given.trimmed();
    };

    for (const CitationItem &item : items) {
        QString type;
        switch (item.kind) {
        case Kind::Article: type = QStringLiteral("JOUR"); break;
        case Kind::Book: type = QStringLiteral("BOOK"); break;
        case Kind::EditedBook: type = QStringLiteral("EDBOOK"); break;
        case Kind::Chapter: type = QStringLiteral("CHAP"); break;
        case Kind::Paper: type = QStringLiteral("CPAPER"); break;
        case Kind::Proceedings: type = QStringLiteral("CONF"); break;
        case Kind::Issue: type = QStringLiteral("JFULL"); break;
        case Kind::Thesis: type = QStringLiteral("THES"); break;
        case Kind::Report: type = QStringLiteral("RPRT"); break;
        case Kind::Standard: type = QStringLiteral("STAND"); break;
        case Kind::Preprint: type = QStringLiteral("UNPB"); break;
        case Kind::Document: type = QStringLiteral("GEN"); break;
        }
        line(QStringLiteral("TY"), type);
        for (const PersonName &person : item.authors)
            line(QStringLiteral("AU"), name(person));
        if (item.kind != Kind::Article) {
            for (const PersonName &person : item.editors)
                line(QStringLiteral("ED"), name(person));
        }
        line(QStringLiteral("TI"), item.kind == Kind::Issue && item.title.isEmpty() ? item.containerTitle : item.title);
        if (item.kind != Kind::Issue || item.containerTitle != item.title)
            line(QStringLiteral("T2"), item.containerTitle);
        line(QStringLiteral("T3"), item.series);
        if (item.year) {
            line(QStringLiteral("PY"), QString::number(*item.year));
            line(QStringLiteral("DA"), QStringLiteral("%1/%2/%3/")
                                           .arg(*item.year, 4, 10, QChar(u'0'))
                                           .arg(item.month ? QStringLiteral("%1").arg(*item.month, 2, 10, QChar(u'0')) : QString())
                                           .arg(item.month && item.day ? QStringLiteral("%1").arg(*item.day, 2, 10, QChar(u'0')) : QString()));
        }
        if (item.kind == Kind::Report)
            line(QStringLiteral("VL"), item.reportNumber.isEmpty() ? item.volume : item.reportNumber);
        else if (item.kind == Kind::Standard)
            line(QStringLiteral("VL"), item.standardNumber);
        else
            line(QStringLiteral("VL"), item.volume);
        line(QStringLiteral("IS"), item.issue);
        if (const std::optional<PageRange> range = pageRange(item.pages)) {
            line(QStringLiteral("SP"), range->first);
            line(QStringLiteral("EP"), range->last);
        } else {
            line(QStringLiteral("SP"), item.pages);
        }
        line(QStringLiteral("ET"), item.edition);
        if (item.kind == Kind::Thesis) {
            line(QStringLiteral("PB"), item.institution.isEmpty() ? item.publisher : item.institution);
            line(QStringLiteral("M3"), item.degree);
        } else if (item.kind == Kind::Report) {
            line(QStringLiteral("PB"), item.institution.isEmpty() ? item.publisher : item.institution);
        } else if (item.kind == Kind::Standard) {
            line(QStringLiteral("PB"), item.publisher.isEmpty() ? item.standardBody : item.publisher);
            line(QStringLiteral("A2"), item.standardBody);
        } else {
            line(QStringLiteral("PB"), item.publisher);
        }
        line(QStringLiteral("CY"), item.kind == Kind::Paper || item.kind == Kind::Proceedings
                                       ? (item.conferenceLocation.isEmpty() ? item.place : item.conferenceLocation)
                                       : item.place);
        if (item.kind == Kind::Paper || item.kind == Kind::Proceedings)
            line(QStringLiteral("C3"), item.conferenceName);
        line(QStringLiteral("DO"), item.doi);
        line(QStringLiteral("UR"), item.url.isEmpty() && !item.arxivId.isEmpty() ? item.link() : item.url);
        line(QStringLiteral("SN"), item.isbn.isEmpty() ? item.issn : item.isbn);
        if (!item.arxivId.isEmpty())
            line(QStringLiteral("M1"), QStringLiteral("arXiv:") + item.arxivId);
        line(QStringLiteral("LA"), item.language);
        for (const QString &keyword : item.keywords)
            line(QStringLiteral("KW"), keyword);
        line(QStringLiteral("AB"), item.abstract);
        out += QStringLiteral("ER  - \r\n\r\n");
    }
    return out.toUtf8();
}

QByteArray toCslJson(const QList<CitationItem> &items)
{
    const QStringList keys = citationKeys(items);
    const auto names = [](const QList<PersonName> &people) {
        QJsonArray array;
        for (const PersonName &person : people) {
            if (isOrganisation(person))
                array.append(QJsonObject{{QStringLiteral("literal"), person.family.trimmed()}});
            else
                array.append(QJsonObject{{QStringLiteral("family"), person.family.trimmed()},
                                         {QStringLiteral("given"), person.given.trimmed()}});
        }
        return array;
    };

    QJsonArray array;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const CitationItem &item = items.at(i);
        QJsonObject object;
        const auto set = [&](const QString &name, const QString &value) {
            if (!value.trimmed().isEmpty())
                object.insert(name, value.trimmed());
        };

        QString type;
        switch (item.kind) {
        case Kind::Article: type = QStringLiteral("article-journal"); break;
        case Kind::Book:
        case Kind::EditedBook:
        case Kind::Proceedings: type = QStringLiteral("book"); break;
        case Kind::Chapter: type = QStringLiteral("chapter"); break;
        case Kind::Paper: type = QStringLiteral("paper-conference"); break;
        case Kind::Issue: type = QStringLiteral("periodical"); break;
        case Kind::Thesis: type = QStringLiteral("thesis"); break;
        case Kind::Report: type = QStringLiteral("report"); break;
        case Kind::Standard: type = QStringLiteral("standard"); break;
        case Kind::Preprint: type = QStringLiteral("article"); break;
        case Kind::Document: type = QStringLiteral("document"); break;
        }
        object.insert(QStringLiteral("id"), keys.at(i));
        object.insert(QStringLiteral("type"), type);
        set(QStringLiteral("title"), item.kind == Kind::Issue && item.title.isEmpty() ? item.containerTitle : item.title);
        if (!item.authors.isEmpty())
            object.insert(QStringLiteral("author"), names(item.authors));
        if (!item.editors.isEmpty() && item.kind != Kind::Article)
            object.insert(QStringLiteral("editor"), names(item.editors));
        if (item.year) {
            QJsonArray parts = {*item.year};
            if (item.month) {
                parts.append(*item.month);
                if (item.day)
                    parts.append(*item.day);
            }
            object.insert(QStringLiteral("issued"), QJsonObject{{QStringLiteral("date-parts"), QJsonArray{parts}}});
        }
        if (item.kind != Kind::Issue || item.containerTitle != item.title)
            set(QStringLiteral("container-title"), item.containerTitle);
        set(QStringLiteral("volume"), item.volume);
        set(QStringLiteral("issue"), item.issue);
        set(QStringLiteral("page"), joinedPages(item.pages, QStringLiteral("-")));
        set(QStringLiteral("edition"), item.edition);
        set(QStringLiteral("collection-title"), item.series);
        if (item.kind == Kind::Thesis)
            set(QStringLiteral("publisher"), item.institution.isEmpty() ? item.publisher : item.institution);
        else if (item.kind == Kind::Report && !item.institution.isEmpty())
            set(QStringLiteral("publisher"), item.institution);
        else if (item.kind == Kind::Preprint && item.publisher.isEmpty() && !item.arxivId.isEmpty())
            set(QStringLiteral("publisher"), QStringLiteral("arXiv"));
        else
            set(QStringLiteral("publisher"), item.publisher);
        set(QStringLiteral("publisher-place"), item.place);
        set(QStringLiteral("event-title"), item.conferenceName);
        set(QStringLiteral("event-place"), item.conferenceLocation);
        if (item.kind == Kind::Thesis)
            set(QStringLiteral("genre"), item.degree);
        if (item.kind == Kind::Report)
            set(QStringLiteral("number"), item.reportNumber);
        if (item.kind == Kind::Standard) {
            set(QStringLiteral("number"), item.standardNumber);
            set(QStringLiteral("authority"), item.standardBody);
        }
        if (item.kind == Kind::Preprint)
            set(QStringLiteral("number"), item.arxivId.isEmpty() ? QString() : QStringLiteral("arXiv:") + item.arxivId);
        set(QStringLiteral("DOI"), item.doi);
        set(QStringLiteral("URL"), item.url.isEmpty() && !item.arxivId.isEmpty() ? item.link() : item.url);
        set(QStringLiteral("ISBN"), item.isbn);
        set(QStringLiteral("ISSN"), item.issn);
        set(QStringLiteral("PMID"), item.pmid);
        set(QStringLiteral("language"), item.language);
        set(QStringLiteral("keyword"), item.keywords.join(QStringLiteral(", ")));
        set(QStringLiteral("abstract"), item.abstract);
        array.append(object);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Indented);
}

QByteArray exportCitations(const QList<CitationItem> &items, ExportFormat format)
{
    switch (format) {
    case ExportFormat::BibTeX: return toBibTeX(items);
    case ExportFormat::Ris: return toRis(items);
    case ExportFormat::CslJson: return toCslJson(items);
    }
    return {};
}

} // namespace papertrail::citation
