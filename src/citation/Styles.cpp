#include "citation/Styles.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <numeric>

namespace papertrail::citation {

using core::PersonName;
using Kind = CitationItem::Kind;

const QList<CitationStyle> &allCitationStyles()
{
    static const QList<CitationStyle> styles = {CitationStyle::Harvard, CitationStyle::Apa, CitationStyle::Ieee,
                                                CitationStyle::Chicago};
    return styles;
}

QString styleKey(CitationStyle style)
{
    switch (style) {
    case CitationStyle::Harvard: return QStringLiteral("harvard");
    case CitationStyle::Apa: return QStringLiteral("apa");
    case CitationStyle::Ieee: return QStringLiteral("ieee");
    case CitationStyle::Chicago: return QStringLiteral("chicago");
    }
    return QStringLiteral("harvard");
}

std::optional<CitationStyle> styleFromKey(const QString &key)
{
    for (const CitationStyle style : allCitationStyles()) {
        if (styleKey(style) == key)
            return style;
    }
    return std::nullopt;
}

QString styleName(CitationStyle style)
{
    switch (style) {
    case CitationStyle::Harvard: return QStringLiteral("Harvard");
    case CitationStyle::Apa: return QStringLiteral("APA (7th edition)");
    case CitationStyle::Ieee: return QStringLiteral("IEEE");
    case CitationStyle::Chicago: return QStringLiteral("Chicago (author-date)");
    }
    return {};
}

namespace {

// Who a reference starts with.
struct Lead {
    QList<PersonName> people;
    bool editors = false;
    bool isEmpty() const { return people.isEmpty(); }
};

PersonName organisation(const QString &name)
{
    return PersonName{name.simplified(), {}};
}

Lead leadFor(const CitationItem &item, bool conferenceCanLead)
{
    if (!item.authors.isEmpty())
        return {item.authors, false};
    // A chapter's, paper's or article's editors are the book's or proceedings', so they don't lead.
    const bool ownEditors = item.kind != Kind::Chapter && item.kind != Kind::Paper && item.kind != Kind::Article;
    if (ownEditors && !item.editors.isEmpty())
        return {item.editors, true};
    if (item.kind == Kind::Standard && !item.standardBody.isEmpty())
        return {{organisation(item.standardBody)}, false};
    if (item.kind == Kind::Report || item.kind == Kind::Document) {
        const QString body = item.institution.isEmpty() ? item.publisher : item.institution;
        if (!body.isEmpty())
            return {{organisation(body)}, false};
    }
    if (conferenceCanLead && item.kind == Kind::Proceedings && !item.conferenceName.isEmpty())
        return {{organisation(item.conferenceName)}, false};
    return {};
}

bool isPart(const CitationItem &item)
{
    return item.kind == Kind::Article || item.kind == Kind::Chapter || item.kind == Kind::Paper;
}

// A title shortened for an in-text citation: what comes before any subtitle.
QString shortTitle(const QString &title)
{
    const qsizetype colon = title.indexOf(u':');
    return colon > 0 ? title.left(colon).trimmed() : title;
}

QString joinWith(const QStringList &names, const QString &separator, const QString &lastSeparator)
{
    if (names.size() <= 1)
        return names.value(0);
    return names.mid(0, names.size() - 1).join(separator) + lastSeparator + names.last();
}

QString yearText(const CitationItem &item)
{
    return item.year ? QString::number(*item.year) : QString();
}

// "Place: Publisher", with s.l. and s.n. standing in for what isn't known, as Harvard asks.
QString harvardPublication(const CitationItem &item, bool always)
{
    if (!always && item.place.isEmpty() && item.publisher.isEmpty())
        return {};
    return (item.place.isEmpty() ? QStringLiteral("s.l.") : item.place) + QStringLiteral(": ")
        + (item.publisher.isEmpty() ? QStringLiteral("s.n.") : item.publisher);
}

QString volumeAndIssue(const CitationItem &item)
{
    if (!item.volume.isEmpty() && !item.issue.isEmpty())
        return item.volume + QStringLiteral(" (") + item.issue + u')';
    if (!item.volume.isEmpty())
        return item.volume;
    if (!item.issue.isEmpty())
        return u'(' + item.issue + u')';
    return {};
}

// ---------------------------------------------------------------------------------------------------- Cases

// A case's name with "v" as its form writes it: "v" for OSCOLA, "v." for the Bluebook.
QString caseName(const CitationItem &item)
{
    QString name = item.title.simplified();
    static const QRegularExpression versus(QStringLiteral("\\s+(?:v|vs)\\.?\\s+"));
    name.replace(versus, item.oscola ? QStringLiteral(" v ") : QStringLiteral(" v. "));
    return name;
}

// Where a report starts, as a case is cited: "562" from "562-580".
QString firstPage(const QString &pages)
{
    static const QRegularExpression range(QStringLiteral("\\s*[-–—]\\s*"));
    return pages.trimmed().section(range, 0, 0);
}

QString oscolaAfterName(const CitationItem &item)
{
    QStringList citations;
    if (!item.neutralCitation.isEmpty())
        citations << item.neutralCitation;
    if (!item.containerTitle.isEmpty()) {
        // Reports organised by year have it in square brackets, then any volume within it.
        QStringList report;
        if (item.year)
            report << u'[' + QString::number(*item.year) + u']';
        if (!item.volume.isEmpty())
            report << item.volume;
        report << item.containerTitle;
        if (!item.pages.isEmpty())
            report << firstPage(item.pages);
        citations << report.join(u' ');
    } else if (item.neutralCitation.isEmpty()) {
        if (item.year)
            citations << u'[' + QString::number(*item.year) + u']';
        if (!item.caseNumber.isEmpty())
            citations << item.caseNumber;
    }
    QString text = item.containerTitle.isEmpty() && item.neutralCitation.isEmpty() ? citations.join(u' ') : citations.join(QStringLiteral(", "));
    // A neutral citation already says which court it was.
    if (item.neutralCitation.isEmpty() && !item.court.isEmpty())
        text += QStringLiteral(" (") + item.court + u')';
    return text.trimmed();
}

QString bluebookDate(const CitationItem &item)
{
    if (!item.year)
        return {};
    static const QStringList months = {QStringLiteral("Jan."), QStringLiteral("Feb."), QStringLiteral("Mar."), QStringLiteral("Apr."),
                                       QStringLiteral("May"),  QStringLiteral("June"), QStringLiteral("July"), QStringLiteral("Aug."),
                                       QStringLiteral("Sept."), QStringLiteral("Oct."), QStringLiteral("Nov."), QStringLiteral("Dec.")};
    // A decision not yet in a report is dated in full.
    if (item.containerTitle.isEmpty() && item.month && *item.month >= 1 && *item.month <= 12) {
        QString date = months.at(*item.month - 1);
        if (item.day)
            date += u' ' + QString::number(*item.day) + u',';
        return date + u' ' + QString::number(*item.year);
    }
    return QString::number(*item.year);
}

QString bluebookAfterName(const CitationItem &item)
{
    QString where;
    if (!item.containerTitle.isEmpty()) {
        QStringList report;
        if (!item.volume.isEmpty())
            report << item.volume;
        report << item.containerTitle;
        if (!item.pages.isEmpty())
            report << firstPage(item.pages);
        where = report.join(u' ');
    } else if (!item.caseNumber.isEmpty()) {
        where = item.caseNumber;
    } else {
        where = item.neutralCitation;
    }
    // The United States Reports are the Supreme Court's own, so they don't name it.
    const bool ownReports = item.containerTitle == QLatin1StringView("U.S.");
    QStringList parenthetical;
    if (!item.court.isEmpty() && !ownReports)
        parenthetical << item.court;
    const QString date = bluebookDate(item);
    if (!date.isEmpty())
        parenthetical << date;
    QString text = where;
    if (!parenthetical.isEmpty())
        text += QStringLiteral(" (") + parenthetical.join(u' ') + u')';
    return text.trimmed();
}

// The case's name and year, as an author-date style cites it in the text, such as "Brown v. Board of Education 1954".
RichText caseInText(const CitationItem &item, const QString &separator, const QString &year)
{
    RichText text;
    text.italic(caseName(item)).add(separator + (year.isEmpty() ? QStringLiteral("n.d.") : year));
    return text;
}

// A title in quotation marks, with the punctuation that follows it inside, unless the title ends in its own.
void quotedTitle(RichText &text, const QString &title, const QString &punctuation)
{
    const bool endsItself = !title.isEmpty() && QStringLiteral("?!").contains(title.back());
    text.add(QStringLiteral("“") + title + (endsItself ? QString() : punctuation) + QStringLiteral("”"));
}

// ---------------------------------------------------------------------------------------------------- Harvard

QString harvardName(const PersonName &person)
{
    if (isOrganisation(person) || isCjkName(person))
        return familyName(person);
    const QString given = initials(person.given, false);
    return given.isEmpty() ? person.family.trimmed() : person.family.trimmed() + QStringLiteral(", ") + given;
}

QString harvardNames(const Lead &lead)
{
    QString text;
    if (lead.people.size() >= 4) {
        text = harvardName(lead.people.first()) + QStringLiteral(", et al.");
    } else {
        QStringList names;
        for (const PersonName &person : lead.people)
            names << harvardName(person);
        text = joinWith(names, QStringLiteral(", "), QStringLiteral(" and "));
    }
    if (lead.editors)
        text += lead.people.size() == 1 ? QStringLiteral(", ed.") : QStringLiteral(", eds.");
    return text;
}

QString harvardInTextNames(const Lead &lead)
{
    if (lead.people.size() >= 4)
        return familyName(lead.people.first()) + QStringLiteral(" et al.");
    QStringList names;
    for (const PersonName &person : lead.people)
        names << familyName(person);
    return joinWith(names, QStringLiteral(", "), QStringLiteral(" and "));
}

// What tells a reader where to find an electronic source, and when it was looked at.
void harvardOnline(RichText &text, const CitationItem &item, const StyleOptions &options)
{
    if (!item.isOnline())
        return;
    const QString accessed = QStringLiteral(" [Accessed ")
        + accessedDate(options.accessed.day(), options.accessed.month(), options.accessed.year()) + u']';
    if (!item.doi.isEmpty())
        text.add(QStringLiteral("DOI: ") + item.doi + accessed);
    else
        text.add(QStringLiteral("Available at: ") + item.link() + accessed);
    text.endSentence().add(QStringLiteral(" "));
}

RichText harvardReference(const CitationItem &item, const QString &year, const StyleOptions &options)
{
    const Lead lead = leadFor(item, true);
    const QString yearLabel = year.isEmpty() ? QStringLiteral("[n.d.]") : year;
    const QString online = item.isOnline() ? QStringLiteral(" [online]") : QString();
    RichText text;

    // A whole issue, or anything with nobody to lead it, starts with its title when it's known by one.
    const bool titleLeads = lead.isEmpty() && item.kind == Kind::Issue;
    if (titleLeads)
        text.italic(item.containerTitle).add(QStringLiteral(", ") + yearLabel + QStringLiteral(". "));
    else
        text.add((lead.isEmpty() ? QStringLiteral("Anon.") : harvardNames(lead)) + QStringLiteral(", ") + yearLabel)
            .endSentence()
            .add(QStringLiteral(" "));

    const QString pages = joinedPages(item.pages, QStringLiteral("-"));
    const QString edition = editionText(item.edition, QStringLiteral("ed."));

    switch (item.kind) {
    case Kind::Article: {
        text.add(item.title).endSentence().add(QStringLiteral(" "));
        if (!item.containerTitle.isEmpty()) {
            text.italic(item.containerTitle).add(online);
            QStringList details;
            QString numbers = volumeAndIssue(item);
            if (item.month)
                numbers += (numbers.isEmpty() ? QString() : QStringLiteral(" ")) + u'(' + englishMonth(*item.month) + u')';
            if (!numbers.isEmpty())
                details << numbers;
            if (!pages.isEmpty())
                details << pages;
            if (!details.isEmpty())
                text.add(QStringLiteral(", ") + details.join(QStringLiteral(", ")));
            text.endSentence().add(QStringLiteral(" "));
        }
        break;
    }
    case Kind::Book:
    case Kind::EditedBook:
        text.italic(item.title).add(online).endSentence().add(QStringLiteral(" "));
        if (!edition.isEmpty())
            text.add(edition).add(QStringLiteral(" "));
        text.add(harvardPublication(item, true)).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Chapter: {
        text.add(item.title).endSentence().add(QStringLiteral(" In: "));
        if (!item.editors.isEmpty())
            text.add(harvardNames({item.editors, true})).endSentence().add(QStringLiteral(" "));
        text.italic(item.containerTitle.isEmpty() ? QStringLiteral("[Untitled]") : item.containerTitle).add(online);
        text.endSentence().add(QStringLiteral(" "));
        if (!edition.isEmpty())
            text.add(edition).add(QStringLiteral(" "));
        text.add(harvardPublication(item, true));
        if (!year.isEmpty())
            text.add(QStringLiteral(", ") + year);
        if (!pages.isEmpty())
            text.add(QStringLiteral(", pp. ") + pages);
        text.endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Paper: {
        text.add(item.title).endSentence().add(QStringLiteral(" In: "));
        if (!item.editors.isEmpty()) {
            text.add(harvardNames({item.editors, true}));
            if (!year.isEmpty())
                text.add(QStringLiteral(", ") + year);
            text.endSentence().add(QStringLiteral(" "));
        }
        text.italic(item.containerTitle.isEmpty() ? QStringLiteral("[Untitled]") : item.containerTitle);
        if (!item.conferenceLocation.isEmpty())
            text.add(QStringLiteral(", ") + item.conferenceLocation);
        text.add(online).endSentence().add(QStringLiteral(" "));
        text.add(harvardPublication(item, true));
        if (!pages.isEmpty())
            text.add(QStringLiteral(", pp. ") + pages);
        text.endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Proceedings:
        text.italic(item.title);
        if (!item.conferenceLocation.isEmpty())
            text.add(QStringLiteral(", ") + item.conferenceLocation);
        text.add(online).endSentence().add(QStringLiteral(" "));
        text.add(harvardPublication(item, true)).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Issue: {
        if (!titleLeads)
            text.italic(item.containerTitle).add(online).endSentence().add(QStringLiteral(" "));
        QString numbers = volumeAndIssue(item);
        if (item.month)
            numbers += (numbers.isEmpty() ? QString() : QStringLiteral(" ")) + u'(' + englishMonth(*item.month) + u')';
        if (!numbers.isEmpty())
            text.add(numbers).endSentence().add(QStringLiteral(" "));
        const QString publication = harvardPublication(item, false);
        if (!publication.isEmpty())
            text.add(publication).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Thesis: {
        text.italic(item.title).add(online).endSentence().add(QStringLiteral(" "));
        const bool dissertation = item.type == core::PublicationType::Dissertation;
        QString type = dissertation ? QStringLiteral("dissertation") : QStringLiteral("thesis");
        type = item.degree.isEmpty() ? (dissertation ? QStringLiteral("Dissertation") : QStringLiteral("Thesis"))
                                     : item.degree + u' ' + type;
        text.add(type);
        if (!item.institution.isEmpty())
            text.add(QStringLiteral(", ") + item.institution);
        text.endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Standard: {
        if (!item.standardNumber.isEmpty()) {
            QString number = item.standardNumber;
            static const QRegularExpression dated(QStringLiteral("[:\\-]\\s*\\d{4}$"));
            if (!year.isEmpty() && !dated.match(number).hasMatch())
                number += QStringLiteral(": ") + yearText(item);
            text.add(number).endSentence().add(QStringLiteral(" "));
        }
        text.italic(item.title).add(online).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Preprint: {
        text.italic(item.title).add(online).endSentence().add(QStringLiteral(" "));
        QString where = QStringLiteral("Preprint");
        if (!item.arxivId.isEmpty())
            where += QStringLiteral(", arXiv:") + item.arxivId;
        else if (!item.publisher.isEmpty())
            where += QStringLiteral(", ") + item.publisher;
        text.add(where).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Report:
    case Kind::Case: // Cited by caseCitation instead, so never here.
    case Kind::Document: {
        text.italic(item.title).add(online).endSentence().add(QStringLiteral(" "));
        const QString publication = harvardPublication(item, false);
        if (!publication.isEmpty())
            text.add(publication).endSentence().add(QStringLiteral(" "));
        if (item.kind == Kind::Report && !item.reportNumber.isEmpty())
            text.add(u'(' + item.reportNumber + QStringLiteral("). "));
        break;
    }
    }
    harvardOnline(text, item, options);
    return text;
}

RichText harvardInText(const CitationItem &item, const QString &year)
{
    if (item.kind == Kind::Case)
        return caseInText(item, QStringLiteral(" "), year);
    const Lead lead = leadFor(item, true);
    RichText text;
    if (lead.isEmpty() && item.kind == Kind::Issue)
        text.italic(item.containerTitle);
    else
        text.add(lead.isEmpty() ? QStringLiteral("Anon.") : harvardInTextNames(lead));
    text.add(u' ' + (year.isEmpty() ? QStringLiteral("n.d.") : year));
    return text;
}

// ---------------------------------------------------------------------------------------------------- APA

QString apaName(const PersonName &person)
{
    if (isOrganisation(person) || isCjkName(person))
        return familyName(person);
    const QString given = initials(person.given, true);
    return given.isEmpty() ? person.family.trimmed() : person.family.trimmed() + QStringLiteral(", ") + given;
}

QString apaInitialsFirst(const PersonName &person)
{
    if (isOrganisation(person) || isCjkName(person))
        return familyName(person);
    const QString given = initials(person.given, true);
    return given.isEmpty() ? person.family.trimmed() : given + u' ' + person.family.trimmed();
}

QString apaNames(const Lead &lead)
{
    QStringList names;
    for (const PersonName &person : lead.people)
        names << apaName(person);
    QString text;
    if (names.size() == 1)
        text = names.first();
    else if (names.size() == 2)
        text = names.at(0) + QStringLiteral(", & ") + names.at(1);
    else if (names.size() <= 20)
        text = names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(", & ") + names.last();
    else
        text = names.mid(0, 19).join(QStringLiteral(", ")) + QStringLiteral(", . . . ") + names.last();
    if (lead.editors)
        text += lead.people.size() == 1 ? QStringLiteral(" (Ed.)") : QStringLiteral(" (Eds.)");
    return text;
}

QString apaEditorsInText(const QList<PersonName> &editors)
{
    QStringList names;
    for (const PersonName &person : editors)
        names << apaInitialsFirst(person);
    QString text;
    if (names.size() == 1)
        text = names.first();
    else if (names.size() == 2)
        text = names.at(0) + QStringLiteral(" & ") + names.at(1);
    else
        text = names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(", & ") + names.last();
    return text + (editors.size() == 1 ? QStringLiteral(" (Ed.)") : QStringLiteral(" (Eds.)"));
}

QString apaInTextNames(const Lead &lead)
{
    if (lead.people.size() >= 3)
        return familyName(lead.people.first()) + QStringLiteral(" et al.");
    QStringList names;
    for (const PersonName &person : lead.people)
        names << familyName(person);
    return names.join(QStringLiteral(" & "));
}

void apaLink(RichText &text, const CitationItem &item)
{
    const QString link = item.link();
    if (!link.isEmpty())
        text.add(link);
}

void apaTitle(RichText &text, const CitationItem &item)
{
    if (isPart(item))
        text.add(item.title);
    else
        text.italic(item.title);
}

RichText apaReference(const CitationItem &item, const QString &year)
{
    const Lead lead = leadFor(item, false);
    const QString yearLabel = year.isEmpty() ? QStringLiteral("n.d.") : year;
    const QString pages = joinedPages(item.pages, QStringLiteral("–"));
    const QString edition = editionText(item.edition, QStringLiteral("ed."));
    RichText text;

    // With nobody to lead, the title moves to the front, and isn't repeated.
    const bool titleLeads = lead.isEmpty();
    if (titleLeads) {
        apaTitle(text, item);
        if (!isPart(item) && !edition.isEmpty())
            text.add(QStringLiteral(" (") + edition + u')');
        text.endSentence().add(QStringLiteral(" (") + yearLabel + QStringLiteral("). "));
    } else {
        text.add(apaNames(lead)).endSentence().add(QStringLiteral(" (") + yearLabel + QStringLiteral("). "));
    }

    const QString publisher = !lead.isEmpty() && lead.people.size() == 1 && isOrganisation(lead.people.first())
            && lead.people.first().family.compare(item.publisher, Qt::CaseInsensitive) == 0
        ? QString() // A publisher that's also the author isn't repeated.
        : item.publisher;

    switch (item.kind) {
    case Kind::Article:
        if (!titleLeads)
            text.add(item.title).endSentence().add(QStringLiteral(" "));
        if (!item.containerTitle.isEmpty()) {
            text.italic(item.containerTitle);
            if (!item.volume.isEmpty())
                text.add(QStringLiteral(", ")).italic(item.volume);
            if (!item.issue.isEmpty())
                text.add((item.volume.isEmpty() ? QStringLiteral(", (") : QStringLiteral("(")) + item.issue + u')');
            if (!pages.isEmpty())
                text.add(QStringLiteral(", ") + pages);
            text.endSentence().add(QStringLiteral(" "));
        }
        break;
    case Kind::Book:
    case Kind::EditedBook:
    case Kind::Proceedings:
    case Kind::Case: // Cited by caseCitation instead, so never here.
    case Kind::Document:
        if (!titleLeads) {
            text.italic(item.title);
            if (!edition.isEmpty() && item.kind != Kind::Document)
                text.add(QStringLiteral(" (") + edition + u')');
            text.endSentence().add(QStringLiteral(" "));
        }
        if (!publisher.isEmpty())
            text.add(publisher).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Chapter:
    case Kind::Paper: {
        if (!titleLeads)
            text.add(item.title).endSentence().add(QStringLiteral(" "));
        text.add(QStringLiteral("In "));
        if (!item.editors.isEmpty())
            text.add(apaEditorsInText(item.editors) + QStringLiteral(", "));
        text.italic(item.containerTitle.isEmpty() ? QStringLiteral("[Untitled]") : item.containerTitle);
        QStringList details;
        if (!edition.isEmpty())
            details << edition;
        if (!pages.isEmpty())
            details << (pageRange(item.pages) && pageRange(item.pages)->isRange() ? QStringLiteral("pp. ") : QStringLiteral("p. ")) + pages;
        if (!details.isEmpty())
            text.add(QStringLiteral(" (") + details.join(QStringLiteral(", ")) + u')');
        text.endSentence().add(QStringLiteral(" "));
        if (!publisher.isEmpty())
            text.add(publisher).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Issue:
        if (!titleLeads)
            text.italic(item.containerTitle).endSentence().add(QStringLiteral(" "));
        if (!item.volume.isEmpty() || !item.issue.isEmpty()) {
            if (!item.volume.isEmpty())
                text.italic(item.volume);
            if (!item.issue.isEmpty())
                text.add(u'(' + item.issue + u')');
            text.endSentence().add(QStringLiteral(" "));
        }
        if (!publisher.isEmpty())
            text.add(publisher).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Thesis: {
        if (!titleLeads)
            text.italic(item.title);
        QString label;
        switch (degreeLevel(item.degree, item.type)) {
        case DegreeLevel::Doctoral: label = QStringLiteral("Doctoral dissertation"); break;
        case DegreeLevel::Masters: label = QStringLiteral("Master’s thesis"); break;
        case DegreeLevel::Unknown:
            label = item.type == core::PublicationType::Dissertation ? QStringLiteral("Dissertation") : QStringLiteral("Thesis");
            break;
        }
        if (!item.institution.isEmpty())
            label += QStringLiteral(", ") + item.institution;
        if (titleLeads) {
            // The title is already at the front, so the description goes after the date.
            text.add(u'[' + label + QStringLiteral("]. "));
        } else {
            text.add(QStringLiteral(" [") + label + QStringLiteral("]. "));
        }
        break;
    }
    case Kind::Report:
    case Kind::Standard: {
        const QString number = item.kind == Kind::Report ? item.reportNumber : item.standardNumber;
        if (!titleLeads) {
            text.italic(item.title);
            if (!number.isEmpty()) {
                bool numeric = false;
                number.toInt(&numeric);
                text.add(QStringLiteral(" (") + (numeric ? QStringLiteral("Report No. ") : QString()) + number + u')');
            }
            text.endSentence().add(QStringLiteral(" "));
        } else if (!number.isEmpty()) {
            text.add(u'(' + number + QStringLiteral("). "));
        }
        if (!publisher.isEmpty())
            text.add(publisher).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Preprint:
        if (!titleLeads)
            text.italic(item.title).add(QStringLiteral(" [Preprint]")).endSentence().add(QStringLiteral(" "));
        else
            text.add(QStringLiteral("[Preprint]. "));
        if (!item.arxivId.isEmpty())
            text.add(QStringLiteral("arXiv. "));
        else if (!publisher.isEmpty())
            text.add(publisher).endSentence().add(QStringLiteral(" "));
        break;
    }
    apaLink(text, item);
    return text;
}

RichText apaInText(const CitationItem &item, const QString &year)
{
    if (item.kind == Kind::Case)
        return caseInText(item, QStringLiteral(", "), year);
    const Lead lead = leadFor(item, false);
    RichText text;
    if (lead.isEmpty()) {
        if (isPart(item))
            quotedTitle(text, shortTitle(item.title), QStringLiteral(","));
        else
            text.italic(shortTitle(item.title)).add(QStringLiteral(","));
    } else {
        text.add(apaInTextNames(lead) + u',');
    }
    text.add(u' ' + (year.isEmpty() ? QStringLiteral("n.d.") : year));
    return text;
}

// ---------------------------------------------------------------------------------------------------- IEEE

QString ieeeName(const PersonName &person)
{
    if (isOrganisation(person) || isCjkName(person))
        return familyName(person);
    const QString given = initials(person.given, true);
    return given.isEmpty() ? person.family.trimmed() : given + u' ' + person.family.trimmed();
}

void ieeeNames(RichText &text, const QList<PersonName> &people, bool editors)
{
    QStringList names;
    for (const PersonName &person : people)
        names << ieeeName(person);
    if (names.size() >= 7) {
        text.add(names.first() + u' ').italic(QStringLiteral("et al."));
    } else if (names.size() == 2) {
        text.add(names.at(0) + QStringLiteral(" and ") + names.at(1));
    } else {
        text.add(joinWith(names, QStringLiteral(", "), QStringLiteral(", and ")));
    }
    if (editors)
        text.add(people.size() == 1 ? QStringLiteral(", Ed.") : QStringLiteral(", Eds."));
}

QString ieeeDate(const CitationItem &item)
{
    if (!item.year)
        return {};
    return item.month ? englishShortMonth(*item.month) + u' ' + QString::number(*item.year) : QString::number(*item.year);
}

QString ieeePages(const CitationItem &item)
{
    const std::optional<PageRange> range = pageRange(item.pages);
    if (!range)
        return item.pages.isEmpty() ? QString() : QStringLiteral("pp. ") + item.pages;
    return range->isRange() ? QStringLiteral("pp. ") + range->first + QStringLiteral("–") + range->last
                            : QStringLiteral("p. ") + range->first;
}

// Joins the parts after a title, ending with the DOI or a web address as IEEE asks.
void ieeeTail(RichText &text, QStringList parts, const CitationItem &item)
{
    if (!item.doi.isEmpty())
        parts << QStringLiteral("doi: ") + item.doi;
    parts.removeAll(QString());
    if (!parts.isEmpty())
        text.add(parts.join(QStringLiteral(", ")));
    text.endSentence();
    if (item.doi.isEmpty() && !item.link().isEmpty())
        text.add(QStringLiteral(" [Online]. Available: ") + item.link());
}

// The author, with the comma that follows. A title takes the lead when there's nobody.
bool ieeeAuthor(RichText &text, const CitationItem &item)
{
    const Lead lead = leadFor(item, false);
    if (lead.isEmpty() || item.kind == Kind::Standard)
        return false;
    ieeeNames(text, lead.people, lead.editors);
    text.add(QStringLiteral(", "));
    return true;
}

RichText ieeeReference(const CitationItem &item)
{
    RichText text;
    const QString edition = editionText(item.edition, QStringLiteral("ed."));
    const QString date = ieeeDate(item);
    const QString year = yearText(item);
    const auto publication = [&] {
        QString place = item.place;
        QString publisher = item.publisher;
        if (place.isEmpty())
            return publisher;
        return publisher.isEmpty() ? place : place + QStringLiteral(": ") + publisher;
    };

    switch (item.kind) {
    case Kind::Article:
        ieeeAuthor(text, item);
        quotedTitle(text, item.title, QStringLiteral(","));
        text.add(QStringLiteral(" "));
        if (!item.containerTitle.isEmpty())
            text.italic(item.containerTitle).add(QStringLiteral(", "));
        ieeeTail(text,
                 {item.volume.isEmpty() ? QString() : QStringLiteral("vol. ") + item.volume,
                  item.issue.isEmpty() ? QString() : QStringLiteral("no. ") + item.issue, ieeePages(item), date},
                 item);
        break;
    case Kind::Book:
    case Kind::EditedBook:
    case Kind::Case: // Cited by caseCitation instead, so never here.
    case Kind::Document: {
        ieeeAuthor(text, item);
        text.italic(item.title);
        if (!edition.isEmpty())
            text.add(QStringLiteral(", ") + edition);
        text.endSentence().add(QStringLiteral(" "));
        const QString where = publication();
        ieeeTail(text, {where.isEmpty() ? year : where + QStringLiteral(", ") + year}, item);
        break;
    }
    case Kind::Chapter: {
        ieeeAuthor(text, item);
        quotedTitle(text, item.title, QStringLiteral(","));
        text.add(QStringLiteral(" in "));
        text.italic(item.containerTitle.isEmpty() ? QStringLiteral("[Untitled]") : item.containerTitle);
        if (!edition.isEmpty())
            text.add(QStringLiteral(", ") + edition);
        if (!item.editors.isEmpty()) {
            text.add(QStringLiteral(", "));
            ieeeNames(text, item.editors, true);
        }
        text.endSentence().add(QStringLiteral(" "));
        const QString where = publication();
        ieeeTail(text, {where.isEmpty() ? year : where + QStringLiteral(", ") + year, ieeePages(item)}, item);
        break;
    }
    case Kind::Paper:
        ieeeAuthor(text, item);
        quotedTitle(text, item.title, QStringLiteral(","));
        text.add(QStringLiteral(" in "));
        text.italic(item.containerTitle.isEmpty() ? QStringLiteral("[Untitled]") : item.containerTitle).add(QStringLiteral(", "));
        ieeeTail(text, {item.conferenceLocation, year, ieeePages(item)}, item);
        break;
    case Kind::Proceedings: {
        ieeeAuthor(text, item);
        text.italic(item.title);
        if (!item.conferenceLocation.isEmpty())
            text.add(QStringLiteral(", ") + item.conferenceLocation);
        text.endSentence().add(QStringLiteral(" "));
        const QString where = publication();
        ieeeTail(text, {where.isEmpty() ? year : where + QStringLiteral(", ") + year}, item);
        break;
    }
    case Kind::Issue:
        text.italic(item.containerTitle).add(QStringLiteral(", "));
        ieeeTail(text,
                 {item.volume.isEmpty() ? QString() : QStringLiteral("vol. ") + item.volume,
                  item.issue.isEmpty() ? QString() : QStringLiteral("no. ") + item.issue, date},
                 item);
        break;
    case Kind::Thesis: {
        ieeeAuthor(text, item);
        quotedTitle(text, item.title, QStringLiteral(","));
        text.add(QStringLiteral(" "));
        QString label;
        switch (degreeLevel(item.degree, item.type)) {
        case DegreeLevel::Doctoral: label = QStringLiteral("Ph.D. dissertation"); break;
        case DegreeLevel::Masters: label = QStringLiteral("M.S. thesis"); break;
        case DegreeLevel::Unknown:
            label = item.type == core::PublicationType::Dissertation ? QStringLiteral("Dissertation") : QStringLiteral("Thesis");
            break;
        }
        ieeeTail(text, {label, item.department, item.institution, item.place, year}, item);
        break;
    }
    case Kind::Report:
        ieeeAuthor(text, item);
        quotedTitle(text, item.title, QStringLiteral(","));
        text.add(QStringLiteral(" "));
        ieeeTail(text,
                 {item.institution.isEmpty() ? item.publisher : item.institution, item.place,
                  item.reportNumber.isEmpty() ? QString() : QStringLiteral("Rep. ") + item.reportNumber, date},
                 item);
        break;
    case Kind::Standard:
        text.italic(item.title).add(QStringLiteral(", "));
        ieeeTail(text, {item.standardNumber, year}, item);
        break;
    case Kind::Preprint:
        ieeeAuthor(text, item);
        quotedTitle(text, item.title, QStringLiteral(","));
        text.add(QStringLiteral(" "));
        ieeeTail(text, {date, item.arxivId.isEmpty() ? item.publisher : QStringLiteral("arXiv:") + item.arxivId}, item);
        break;
    }
    return text;
}

// ---------------------------------------------------------------------------------------------------- Chicago

QString chicagoName(const PersonName &person, bool inverted)
{
    if (isOrganisation(person) || isCjkName(person))
        return familyName(person);
    if (person.given.trimmed().isEmpty())
        return person.family.trimmed();
    return inverted ? person.family.trimmed() + QStringLiteral(", ") + person.given.trimmed()
                    : person.given.trimmed() + u' ' + person.family.trimmed();
}

QString chicagoNames(const Lead &lead)
{
    QStringList names;
    for (qsizetype i = 0; i < lead.people.size(); ++i)
        names << chicagoName(lead.people.at(i), i == 0);
    QString text;
    if (names.size() >= 7)
        text = names.mid(0, 3).join(QStringLiteral(", ")) + QStringLiteral(", et al.");
    else if (names.size() == 2)
        text = names.at(0) + QStringLiteral(", and ") + names.at(1);
    else
        text = joinWith(names, QStringLiteral(", "), QStringLiteral(", and "));
    if (lead.editors)
        text += lead.people.size() == 1 ? QStringLiteral(", ed.") : QStringLiteral(", eds.");
    return text;
}

QString chicagoEditors(const QList<PersonName> &editors)
{
    QStringList names;
    for (const PersonName &person : editors)
        names << chicagoName(person, false);
    if (names.size() == 2)
        return names.at(0) + QStringLiteral(" and ") + names.at(1);
    return joinWith(names, QStringLiteral(", "), QStringLiteral(", and "));
}

QString chicagoInTextNames(const Lead &lead)
{
    if (lead.people.size() >= 4)
        return familyName(lead.people.first()) + QStringLiteral(" et al.");
    QStringList names;
    for (const PersonName &person : lead.people)
        names << familyName(person);
    if (names.size() == 2)
        return names.at(0) + QStringLiteral(" and ") + names.at(1);
    return joinWith(names, QStringLiteral(", "), QStringLiteral(", and "));
}

void chicagoLink(RichText &text, const CitationItem &item)
{
    const QString link = item.link();
    if (!link.isEmpty())
        text.add(link).endSentence();
}

void chicagoTitle(RichText &text, const CitationItem &item)
{
    if (isPart(item) || item.kind == Kind::Thesis || item.kind == Kind::Preprint)
        quotedTitle(text, item.title, QStringLiteral("."));
    else
        text.italic(item.title).endSentence();
}

RichText chicagoReference(const CitationItem &item, const QString &year)
{
    const Lead lead = leadFor(item, false);
    const QString yearLabel = year.isEmpty() ? QStringLiteral("n.d.") : year;
    const QString edition = editionText(item.edition, QStringLiteral("ed."));
    RichText text;

    const bool titleLeads = lead.isEmpty();
    if (titleLeads) {
        if (item.kind == Kind::Issue)
            text.italic(item.containerTitle).endSentence();
        else
            chicagoTitle(text, item);
        text.add(u' ' + yearLabel).endSentence().add(QStringLiteral(" "));
    } else {
        text.add(chicagoNames(lead)).endSentence().add(u' ' + yearLabel).endSentence().add(QStringLiteral(" "));
        if (item.kind != Kind::Issue) {
            chicagoTitle(text, item);
            text.add(QStringLiteral(" "));
        }
    }

    switch (item.kind) {
    case Kind::Article:
        if (!item.containerTitle.isEmpty()) {
            text.italic(item.containerTitle);
            if (!item.volume.isEmpty())
                text.add(u' ' + item.volume);
            if (!item.issue.isEmpty())
                text.add(item.volume.isEmpty() ? QStringLiteral(", no. ") + item.issue : QStringLiteral(" (") + item.issue + u')');
            if (!item.pages.isEmpty())
                text.add(QStringLiteral(": ") + chicagoPages(item.pages));
            text.endSentence().add(QStringLiteral(" "));
        }
        break;
    case Kind::Book:
    case Kind::EditedBook:
    case Kind::Proceedings:
    case Kind::Case: // Cited by caseCitation instead, so never here.
    case Kind::Document:
        if (!edition.isEmpty() && item.kind != Kind::Document)
            text.add(edition + u' ');
        if (!item.publisher.isEmpty())
            text.add(item.publisher).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Chapter:
    case Kind::Paper:
        text.add(QStringLiteral("In "));
        text.italic(item.containerTitle.isEmpty() ? QStringLiteral("[Untitled]") : item.containerTitle);
        if (!edition.isEmpty())
            text.add(QStringLiteral(", ") + edition);
        if (!item.editors.isEmpty())
            text.add(QStringLiteral(", edited by ") + chicagoEditors(item.editors));
        if (!item.pages.isEmpty())
            text.add(QStringLiteral(", ") + chicagoPages(item.pages));
        text.endSentence().add(QStringLiteral(" "));
        if (!item.publisher.isEmpty())
            text.add(item.publisher).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Issue:
        if (!titleLeads)
            text.italic(item.containerTitle);
        if (!item.volume.isEmpty())
            text.add((titleLeads ? QString() : QStringLiteral(" ")) + item.volume);
        if (!item.issue.isEmpty())
            text.add(item.volume.isEmpty() ? QStringLiteral("no. ") + item.issue : QStringLiteral(" (") + item.issue + u')');
        text.endSentence().add(QStringLiteral(" "));
        if (!item.publisher.isEmpty())
            text.add(item.publisher).endSentence().add(QStringLiteral(" "));
        break;
    case Kind::Thesis: {
        QString label;
        switch (degreeLevel(item.degree, item.type)) {
        case DegreeLevel::Doctoral: label = QStringLiteral("PhD diss."); break;
        case DegreeLevel::Masters: label = QStringLiteral("Master’s thesis"); break;
        case DegreeLevel::Unknown:
            label = item.type == core::PublicationType::Dissertation ? QStringLiteral("Dissertation") : QStringLiteral("Thesis");
            break;
        }
        text.add(label);
        if (!item.institution.isEmpty())
            text.add(QStringLiteral(", ") + item.institution);
        text.endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Report: {
        if (!item.reportNumber.isEmpty()) {
            bool numeric = false;
            item.reportNumber.toInt(&numeric);
            text.add((numeric ? QStringLiteral("Report no. ") : QString()) + item.reportNumber).endSentence().add(QStringLiteral(" "));
        }
        const bool publisherLeads = lead.people.size() == 1 && isOrganisation(lead.people.first())
            && lead.people.first().family.compare(item.publisher, Qt::CaseInsensitive) == 0;
        if (!item.publisher.isEmpty() && !publisherLeads)
            text.add(item.publisher).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Standard: {
        if (!item.standardNumber.isEmpty())
            text.add(item.standardNumber).endSentence().add(QStringLiteral(" "));
        const bool sameBody = item.publisher.compare(item.standardBody, Qt::CaseInsensitive) == 0;
        if (!item.publisher.isEmpty() && !sameBody)
            text.add(item.publisher).endSentence().add(QStringLiteral(" "));
        break;
    }
    case Kind::Preprint:
        text.add(QStringLiteral("Preprint"));
        if (!item.arxivId.isEmpty())
            text.add(QStringLiteral(", arXiv"));
        else if (!item.publisher.isEmpty())
            text.add(QStringLiteral(", ") + item.publisher);
        text.endSentence().add(QStringLiteral(" "));
        break;
    }
    chicagoLink(text, item);
    return text;
}

RichText chicagoInText(const CitationItem &item, const QString &year)
{
    if (item.kind == Kind::Case)
        return caseInText(item, QStringLiteral(" "), year);
    const Lead lead = leadFor(item, false);
    RichText text;
    if (lead.isEmpty()) {
        if (item.kind == Kind::Issue)
            text.italic(item.containerTitle);
        else if (isPart(item) || item.kind == Kind::Thesis || item.kind == Kind::Preprint)
            quotedTitle(text, shortTitle(item.title), QString());
        else
            text.italic(shortTitle(item.title));
    } else {
        text.add(chicagoInTextNames(lead));
    }
    text.add(u' ' + (year.isEmpty() ? QStringLiteral("n.d.") : year));
    return text;
}

// ---------------------------------------------------------------------------------------------------- Lists

// The names an in-text citation shows, which also decide which works need letters after their years.
QString disambiguationKey(const CitationItem &item, CitationStyle style)
{
    RichText names;
    switch (style) {
    case CitationStyle::Harvard: names = harvardInText(item, QString()); break;
    case CitationStyle::Apa: names = apaInText(item, QString()); break;
    case CitationStyle::Chicago: names = chicagoInText(item, QString()); break;
    case CitationStyle::Ieee: return {};
    }
    // Every author counts, so works by different groups that share a first author keep their own years.
    QStringList everyone;
    const Lead lead = leadFor(item, style == CitationStyle::Harvard);
    for (const PersonName &person : lead.people)
        everyone << sortKey(person.family + u' ' + person.given);
    return sortKey(names.plain()) + u'|' + everyone.join(u';') + u'|' + yearText(item);
}

QString leadSortText(const CitationItem &item, CitationStyle style)
{
    if (item.kind == Kind::Case)
        return sortKey(caseName(item));
    const Lead lead = leadFor(item, style == CitationStyle::Harvard);
    if (lead.isEmpty()) {
        if (style == CitationStyle::Harvard && item.kind != Kind::Issue)
            return QStringLiteral("anon");
        QString title = item.kind == Kind::Issue ? item.containerTitle : item.title;
        // An article at the start of a title doesn't count when alphabetising.
        static const QRegularExpression article(QStringLiteral("^(the|a|an)\\s+"), QRegularExpression::CaseInsensitiveOption);
        return sortKey(title.remove(article));
    }
    QStringList names;
    for (const PersonName &person : lead.people)
        names << sortKey(familyName(person) + u' ' + person.given);
    return names.join(u' ');
}

} // namespace

RichText caseCitation(const CitationItem &item)
{
    RichText text;
    text.italic(caseName(item));
    const QString after = caseCitationAfterName(item);
    if (!after.isEmpty())
        text.add((item.oscola ? QStringLiteral(" ") : QStringLiteral(", ")) + after);
    return text;
}

QString caseCitationAfterName(const CitationItem &item)
{
    return item.oscola ? oscolaAfterName(item) : bluebookAfterName(item);
}

FormattedReferences formatReferences(const QList<CitationItem> &items, CitationStyle style, const StyleOptions &options)
{
    FormattedReferences result;
    QList<int> order(items.size());
    std::iota(order.begin(), order.end(), 0);

    // Author-date styles are alphabetical. IEEE keeps the order it's given, and numbers it.
    if (style != CitationStyle::Ieee) {
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const CitationItem &x = items.at(a);
            const CitationItem &y = items.at(b);
            const QString xLead = leadSortText(x, style);
            const QString yLead = leadSortText(y, style);
            if (xLead != yLead)
                return xLead < yLead;
            const int xYear = x.year.value_or(0);
            const int yYear = y.year.value_or(0);
            if (xYear != yYear)
                return xYear < yYear;
            return sortKey(x.title) < sortKey(y.title);
        });
    }

    // Letters for works sharing names and a year, given in the list's order.
    QHash<int, QString> years;
    if (style != CitationStyle::Ieee) {
        QHash<QString, QList<int>> groups;
        for (const int index : std::as_const(order))
            groups[disambiguationKey(items.at(index), style)] << index;
        for (const QList<int> &group : std::as_const(groups)) {
            QSet<QString> distinct;
            for (const int index : group)
                distinct.insert(items.at(index).uuid.isEmpty() ? QString::number(index) : items.at(index).uuid);
            const bool letters = group.size() > 1 && distinct.size() > 1;
            for (qsizetype i = 0; i < group.size(); ++i) {
                const CitationItem &item = items.at(group.at(i));
                QString year = yearText(item);
                if (letters && !year.isEmpty())
                    year += QChar(u'a' + int(qMin<qsizetype>(i, 25)));
                years.insert(group.at(i), year);
            }
        }
    }

    result.order = order;
    for (qsizetype position = 0; position < order.size(); ++position) {
        const int index = order.at(position);
        const CitationItem &item = items.at(index);
        RichText entry;
        if (item.kind == Kind::Case) {
            // The same legal form in every style.
            entry = style == CitationStyle::Ieee ? RichText().add(QStringLiteral("[%1] ").arg(position + 1)).append(caseCitation(item))
                                                 : caseCitation(item);
            result.entries << entry;
            continue;
        }
        switch (style) {
        case CitationStyle::Harvard: entry = harvardReference(item, years.value(index), options); break;
        case CitationStyle::Apa: entry = apaReference(item, years.value(index)); break;
        case CitationStyle::Chicago: entry = chicagoReference(item, years.value(index)); break;
        case CitationStyle::Ieee: entry = RichText().add(QStringLiteral("[%1] ").arg(position + 1)).append(ieeeReference(item)); break;
        }
        result.entries << entry;
    }

    for (qsizetype index = 0; index < items.size(); ++index) {
        RichText cite;
        switch (style) {
        case CitationStyle::Harvard:
            cite.add(QStringLiteral("(")).append(harvardInText(items.at(index), years.value(int(index)))).add(QStringLiteral(")"));
            break;
        case CitationStyle::Apa:
            cite.add(QStringLiteral("(")).append(apaInText(items.at(index), years.value(int(index)))).add(QStringLiteral(")"));
            break;
        case CitationStyle::Chicago:
            cite.add(QStringLiteral("(")).append(chicagoInText(items.at(index), years.value(int(index)))).add(QStringLiteral(")"));
            break;
        case CitationStyle::Ieee: cite.add(QStringLiteral("[%1]").arg(index + 1)); break;
        }
        result.inText << cite;
    }
    return result;
}

RichText combinedInTextCitation(const QList<CitationItem> &items, CitationStyle style, const StyleOptions &options)
{
    const FormattedReferences references = formatReferences(items, style, options);
    RichText text;
    if (items.isEmpty())
        return text;

    if (style == CitationStyle::Ieee) {
        // They're numbered in the order given, so three or more make a range, as in [1]–[3].
        const int count = int(items.size());
        if (count >= 3)
            return text.add(QStringLiteral("[1]–[%1]").arg(count));
        QStringList parts;
        for (int n = 1; n <= count; ++n)
            parts << QStringLiteral("[%1]").arg(n);
        return text.add(parts.join(QStringLiteral(", ")));
    }

    // One pair of brackets around them all, in the reference list's order.
    text.add(QStringLiteral("("));
    for (qsizetype i = 0; i < references.order.size(); ++i) {
        if (i > 0)
            text.add(QStringLiteral("; "));
        RichText one = references.inText.at(references.order.at(i));
        // Without its own brackets.
        RichText inner;
        const QList<RichText::Run> &runs = one.runs();
        for (qsizetype r = 0; r < runs.size(); ++r) {
            QString piece = runs.at(r).text;
            if (r == 0 && piece.startsWith(u'('))
                piece.remove(0, 1);
            if (r == runs.size() - 1 && piece.endsWith(u')'))
                piece.chop(1);
            inner.add(piece, runs.at(r).italic);
        }
        text.append(inner);
    }
    text.add(QStringLiteral(")"));
    return text;
}

} // namespace papertrail::citation
