#include "metadata/CaseLaw.h"

#include "core/model/Identifiers.h"
#include "metadata/Matching.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace papertrail::metadata {

using namespace core;

const QString kFindCaseLaw = QStringLiteral("findcaselaw");
const QString kCourtListener = QStringLiteral("courtlistener");
const QString kEurLex = QStringLiteral("eurlex");
const QString kCanLii = QStringLiteral("canlii");

namespace {

constexpr int kMostResults = 10;
constexpr qsizetype kLongestText = 500; // Nothing a source says about a case needs more than this.

QString cleaned(const QString &text)
{
    return text.simplified().left(kLongestText);
}

// The neutral citations Find Case Law holds, with their court's jurisdiction.
const QRegularExpression &findCaseLawCitation()
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\[((?:19|20)\\d\\d)\\]\\s+(UKSC|UKPC|UKHL|EWCA\\s+Civ|EWCA\\s+Crim|EWHC|EWFC|EWCOP|UKUT|UKFTT|UKEAT|EAT|"
                       "UKIPTrib|UKAIT|EWCR)\\s+(\\d{1,5})(?:\\s*\\(([A-Za-z]+)\\))?$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

// Neutral citations of UK courts Find Case Law doesn't hold, and of other common-law courts, which no free source here
// covers.
const QRegularExpression &otherCommonLawCitation()
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\[(?:19|20)\\d\\d\\]\\s+(CSIH|CSOH|HCJAC|SAC|NICA|NIQB|NICh|IESC|IECA|IEHC|HCA|FCA|FCAFC|NZSC|NZCA|NZHC)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

struct CanadianCourt {
    const char *code;
    const char *database;
    const char *court;
};

const CanadianCourt kCanadianCourts[] = {
    {"SCC", "csc-scc", "Supreme Court of Canada"},
    {"FCA", "fca", "Federal Court of Appeal"},
    {"FC", "fct", "Federal Court"},
    {"TCC", "tcc", "Tax Court of Canada"},
    {"ONCA", "onca", "Court of Appeal for Ontario"},
    {"ONSC", "onsc", "Ontario Superior Court of Justice"},
    {"BCCA", "bcca", "Court of Appeal for British Columbia"},
    {"BCSC", "bcsc", "Supreme Court of British Columbia"},
    {"ABCA", "abca", "Court of Appeal of Alberta"},
    {"ABKB", "abkb", "Court of King's Bench of Alberta"},
    {"ABQB", "abqb", "Court of Queen's Bench of Alberta"},
    {"QCCA", "qcca", "Court of Appeal of Quebec"},
    {"QCCS", "qccs", "Superior Court of Quebec"},
    {"MBCA", "mbca", "Court of Appeal of Manitoba"},
    {"SKCA", "skca", "Court of Appeal for Saskatchewan"},
    {"NSCA", "nsca", "Nova Scotia Court of Appeal"},
    {"NSSC", "nssc", "Supreme Court of Nova Scotia"},
    {"NBCA", "nbca", "Court of Appeal of New Brunswick"},
    {"NLCA", "nlca", "Court of Appeal of Newfoundland and Labrador"},
    {"PECA", "peca", "Court of Appeal of Prince Edward Island"},
};

const QRegularExpression &canadianCitation()
{
    static const QRegularExpression pattern(QStringLiteral("^((?:19|20)\\d\\d)\\s+([A-Z]{2,4})\\s+(\\d{1,6})(?:\\s*\\(CanLII\\))?$"));
    return pattern;
}

const CanadianCourt *canadianCourt(const QString &code)
{
    for (const CanadianCourt &court : kCanadianCourts) {
        if (code == QLatin1StringView(court.code))
            return &court;
    }
    return nullptr;
}

// Reporters that are American, so a law report in one says where a case is from.
bool isUsReporter(const QString &reporter)
{
    static const QRegularExpression us(QStringLiteral("^(U\\.\\s?S\\.|S\\.\\s?Ct\\.|L\\.\\s?Ed\\.(\\s?2d)?|F\\.(\\s?(2d|3d|4th))?|"
                                                      "F\\.\\s?Supp\\.(\\s?(2d|3d))?|F\\.\\s?App'x|B\\.R\\.|"
                                                      "(A|P|N\\.E|N\\.W|S\\.E|S\\.W|So)\\.(\\s?(2d|3d))?|Cal\\.(\\s?Rptr\\.)?.*|"
                                                      "N\\.Y\\.S\\.(\\s?(2d|3d))?)$"));
    return us.match(reporter.simplified()).hasMatch();
}

bool containsAnyOf(const QString &text, const QStringList &needles)
{
    return std::any_of(needles.cbegin(), needles.cend(), [&](const QString &needle) { return text.contains(needle, Qt::CaseInsensitive); });
}

QString normalisedCitation(const QString &citation)
{
    QString text = citation.simplified().toUpper();
    text.remove(QStringLiteral(" (CANLII)"));
    return text;
}

// Only a page on the source's own site is kept as a case's address.
QString sitePage(const QString &base, const QString &address)
{
    QString full = address.trimmed();
    if (full.startsWith(u'/') && !full.startsWith(QStringLiteral("//")))
        full = base + full;
    const QUrl url(full, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1StringView("https") || !url.userInfo().isEmpty()
        || url.host() != QUrl(base).host() || full.size() > 300) {
        return QString();
    }
    return url.toString(QUrl::FullyEncoded);
}

void setDate(Publication &publication, const QString &text)
{
    static const QRegularExpression date(QStringLiteral("^((?:1[6-9]|20)\\d\\d)-(\\d\\d)-(\\d\\d)"));
    const QRegularExpressionMatch match = date.match(text.trimmed());
    if (!match.hasMatch())
        return;
    const int month = match.captured(2).toInt();
    const int day = match.captured(3).toInt();
    publication.year = match.captured(1).toInt();
    if (month >= 1 && month <= 12) {
        publication.month = month;
        if (day >= 1 && day <= 31)
            publication.day = day;
    }
}

FoundRecord caseRecord(const QString &provider)
{
    FoundRecord record;
    record.provider = provider;
    record.type = PublicationType::CaseLaw;
    record.publication.type = PublicationType::CaseLaw;
    return record;
}

QString jurisdictionOfFindCaseLawCitation(const QString &citation)
{
    const QRegularExpressionMatch match = findCaseLawCitation().match(citation.simplified());
    if (!match.hasMatch())
        return QString();
    const QString code = match.captured(2).toUpper();
    return code.startsWith(QStringLiteral("EW")) ? QStringLiteral("England and Wales") : QStringLiteral("United Kingdom");
}

// A case's name as the words of its parties alone, leaving out what differs from one way of writing it to another. "v"
// and the Crown are left out too, since they're in so many names that they'd make different cases look alike.
QString caseNameKey(const QString &name)
{
    QString key = foldedText(name);
    static const QRegularExpression punctuation(QStringLiteral("[^\\p{L}\\p{N}&]+"));
    key.replace(punctuation, QStringLiteral(" "));
    key = QStringLiteral(" ") + key + QStringLiteral(" ");
    static const QList<QPair<QRegularExpression, QString>> forms = {
        {QRegularExpression(QStringLiteral("\\s(vs?|versus)\\s")), QStringLiteral(" ")},
        {QRegularExpression(QStringLiteral("\\s(r|regina|rex|the queen|the king|the crown)\\s")), QStringLiteral(" ")},
        {QRegularExpression(QStringLiteral("\\s(and|&)\\s(others|another|ors|anor)\\s")), QStringLiteral(" ")},
        {QRegularExpression(QStringLiteral("\\set\\sal\\s")), QStringLiteral(" ")},
        {QRegularExpression(QStringLiteral("\\s(ltd|limited|inc|incorporated|plc|llc|llp|corp|corporation|co|company|sa|ag|bv|gmbh)\\s")),
         QStringLiteral(" ")},
        {QRegularExpression(QStringLiteral("\\s(the|of)\\s")), QStringLiteral(" ")},
    };
    // Twice, since one match's space is the next one's.
    for (int round = 0; round < 2; ++round) {
        for (const auto &form : forms)
            key.replace(form.first, form.second);
    }
    return key.simplified();
}

QString euCaseNumber(const QString &text)
{
    static const QRegularExpression number(QStringLiteral("\\b([CT])\\s*-\\s*(\\d{1,4})\\s*/\\s*(\\d\\d)\\b"));
    const QRegularExpressionMatch match = number.match(text.toUpper());
    if (!match.hasMatch())
        return QString();
    return match.captured(1) + u'-' + QString::number(match.captured(2).toInt()) + u'/' + match.captured(3);
}

} // namespace

// ---- Which source

CaseJurisdiction caseJurisdictionOf(const Publication &publication)
{
    const QString citation = publication.neutralCitation.simplified();
    if (!citation.isEmpty()) {
        if (!euEcli(citation).isEmpty())
            return CaseJurisdiction::EuropeanUnion;
        if (isFindCaseLawCitation(citation))
            return CaseJurisdiction::UnitedKingdom;
        if (canLiiCaseFor(citation))
            return CaseJurisdiction::Canada;
        if (otherCommonLawCitation().match(citation).hasMatch())
            return CaseJurisdiction::Unknown;
    }
    if (!euCaseNumber(publication.caseNumber).isEmpty())
        return CaseJurisdiction::EuropeanUnion;

    const QString jurisdiction = publication.jurisdiction;
    if (containsAnyOf(jurisdiction, {QStringLiteral("Scotland"), QStringLiteral("Northern Ireland"), QStringLiteral("Ireland"),
                                     QStringLiteral("Australia"), QStringLiteral("New Zealand")})) {
        return CaseJurisdiction::Unknown;
    }
    if (containsAnyOf(jurisdiction, {QStringLiteral("England"), QStringLiteral("Wales"), QStringLiteral("United Kingdom")}))
        return CaseJurisdiction::UnitedKingdom;
    if (containsAnyOf(jurisdiction, {QStringLiteral("United States"), QStringLiteral("U.S.")}))
        return CaseJurisdiction::UnitedStates;
    if (containsAnyOf(jurisdiction, {QStringLiteral("European Union")}))
        return CaseJurisdiction::EuropeanUnion;
    if (containsAnyOf(jurisdiction, {QStringLiteral("Canada")}))
        return CaseJurisdiction::Canada;

    const QString court = publication.court;
    if (containsAnyOf(court, {QStringLiteral("Court of Justice of the European"), QStringLiteral("General Court")}))
        return CaseJurisdiction::EuropeanUnion;
    static const QRegularExpression usCourt(QStringLiteral("^(U\\.S\\.|\\d+(st|nd|d|rd|th) Cir\\.|D\\.C\\. Cir\\.|Fed\\. Cir\\.|"
                                                           "[A-Z][A-Za-z.]* ?D\\. ?[A-Z][A-Za-z.]*)$"));
    if (usCourt.match(court.simplified()).hasMatch() || court.contains(QStringLiteral("United States")))
        return CaseJurisdiction::UnitedStates;
    if (isUsReporter(publication.containerTitle))
        return CaseJurisdiction::UnitedStates;

    if (publication.caseCitationForm == QLatin1StringView("us"))
        return CaseJurisdiction::UnitedStates;
    if (publication.caseCitationForm == QLatin1StringView("uk"))
        return CaseJurisdiction::UnitedKingdom;
    return CaseJurisdiction::Unknown;
}

// ---- Find Case Law

bool isFindCaseLawCitation(const QString &citation)
{
    return findCaseLawCitation().match(citation.simplified()).hasMatch();
}

QUrl findCaseLawSearchUrl(const ProviderUrls &urls, const QString &query)
{
    QUrl url(urls.findCaseLaw + QStringLiteral("/atom.xml"));
    QUrlQuery items;
    items.addQueryItem(QStringLiteral("query"), QString::fromLatin1(QUrl::toPercentEncoding(query.simplified().left(200))));
    items.addQueryItem(QStringLiteral("order"), QStringLiteral("relevance"));
    items.addQueryItem(QStringLiteral("per_page"), QString::number(kMostResults));
    url.setQuery(items);
    return url;
}

QList<FoundRecord> parseFindCaseLaw(const QByteArray &body)
{
    QList<FoundRecord> records;
    QXmlStreamReader xml(body);
    std::optional<FoundRecord> entry;
    bool inAuthor = false;
    while (!xml.atEnd() && records.size() < kMostResults) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QLatin1StringView("entry")) {
                entry = caseRecord(kFindCaseLaw);
                entry->publication.caseCitationForm = QStringLiteral("uk");
            } else if (!entry) {
                continue;
            } else if (name == QLatin1StringView("author")) {
                inAuthor = true;
            } else if (name == QLatin1StringView("name") && inAuthor) {
                entry->publication.court = cleaned(xml.readElementText());
            } else if (name == QLatin1StringView("title")) {
                entry->publication.title = cleaned(xml.readElementText());
            } else if (name == QLatin1StringView("published")) {
                setDate(entry->publication, xml.readElementText());
            } else if (name == QLatin1StringView("link")) {
                const auto attributes = xml.attributes();
                if (attributes.value(QStringLiteral("rel")) == QLatin1StringView("alternate") && attributes.value(QStringLiteral("type")).isEmpty())
                    entry->publication.url = sitePage(QStringLiteral("https://caselaw.nationalarchives.gov.uk"),
                                                      attributes.value(QStringLiteral("href")).toString());
            } else if (name == QLatin1StringView("identifier")
                       && xml.attributes().value(QStringLiteral("type")) == QLatin1StringView("ukncn")) {
                const QString citation = cleaned(xml.readElementText());
                if (isFindCaseLawCitation(citation)) {
                    entry->publication.neutralCitation = citation;
                    entry->publication.jurisdiction = jurisdictionOfFindCaseLawCitation(citation);
                }
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1StringView("author")) {
                inAuthor = false;
            } else if (xml.name() == QLatin1StringView("entry") && entry) {
                if (!entry->publication.title.isEmpty())
                    records << *entry;
                entry.reset();
            }
        }
    }
    return records;
}

// ---- CourtListener

QString reporterCitationOf(const Publication &publication)
{
    const QString reporter = publication.containerTitle.simplified();
    const QString volume = publication.volume.simplified();
    const QString page = publication.pages.section(u'-', 0, 0).section(u'–', 0, 0).simplified();
    static const QRegularExpression digits(QStringLiteral("^\\d{1,5}$"));
    if (reporter.isEmpty() || !digits.match(volume).hasMatch() || !digits.match(page).hasMatch())
        return QString();
    return volume + u' ' + reporter + u' ' + page;
}

namespace {

QString courtListenerSafe(const QString &text)
{
    // Inside quotes in its search, so nothing that could end them or start an operator.
    QString safe = text.simplified().left(200);
    static const QRegularExpression unsafe(QStringLiteral("[\"()\\[\\]{}:\\\\^~*?]"));
    safe.replace(unsafe, QStringLiteral(" "));
    return safe.simplified();
}

QUrl courtListenerSearch(const ProviderUrls &urls, const QString &q, std::optional<int> year)
{
    QUrl url(urls.courtListener + QStringLiteral("/api/rest/v4/search/"));
    QUrlQuery items;
    items.addQueryItem(QStringLiteral("type"), QStringLiteral("o"));
    items.addQueryItem(QStringLiteral("q"), QString::fromLatin1(QUrl::toPercentEncoding(q)));
    items.addQueryItem(QStringLiteral("order_by"), QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("score desc"))));
    if (year) {
        items.addQueryItem(QStringLiteral("filed_after"), QStringLiteral("%1-01-01").arg(*year - 1));
        items.addQueryItem(QStringLiteral("filed_before"), QStringLiteral("%1-12-31").arg(*year + 1));
    }
    url.setQuery(items);
    return url;
}

// The Bluebook's abbreviation for the federal courts of appeals and the Supreme Court, by CourtListener's ids.
QString bluebookCourt(const QString &courtId)
{
    if (courtId == QLatin1StringView("scotus"))
        return QStringLiteral("U.S.");
    if (courtId == QLatin1StringView("cadc"))
        return QStringLiteral("D.C. Cir.");
    if (courtId == QLatin1StringView("cafc"))
        return QStringLiteral("Fed. Cir.");
    static const QRegularExpression circuit(QStringLiteral("^ca(\\d{1,2})$"));
    const QRegularExpressionMatch match = circuit.match(courtId);
    if (match.hasMatch()) {
        const int n = match.captured(1).toInt();
        if (n < 1 || n > 11)
            return QString();
        const QString suffix = n == 1 ? QStringLiteral("st") : n == 2 ? QStringLiteral("d") : n == 3 ? QStringLiteral("d") : QStringLiteral("th");
        return QStringLiteral("%1%2 Cir.").arg(n).arg(suffix);
    }
    return QString();
}

// The official report first, then the Supreme Court Reporter, then the Lawyers' Edition, then whatever's first.
QString preferredCitation(const QStringList &citations)
{
    for (const QString &reporter : {QStringLiteral(" U.S. "), QStringLiteral(" S. Ct. "), QStringLiteral(" L. Ed.")}) {
        for (const QString &citation : citations) {
            if (citation.contains(reporter))
                return citation;
        }
    }
    return citations.isEmpty() ? QString() : citations.first();
}

} // namespace

QUrl courtListenerCitationUrl(const ProviderUrls &urls, const QString &reporterCitation)
{
    return courtListenerSearch(urls, QStringLiteral("citation:(\"%1\")").arg(courtListenerSafe(reporterCitation)), std::nullopt);
}

QUrl courtListenerNameUrl(const ProviderUrls &urls, const QString &name, std::optional<int> year)
{
    return courtListenerSearch(urls, QStringLiteral("caseName:(\"%1\")").arg(courtListenerSafe(name)), year);
}

QList<FoundRecord> parseCourtListenerSearch(const QByteArray &body)
{
    QList<FoundRecord> records;
    const QJsonArray results = QJsonDocument::fromJson(body).object().value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : results) {
        if (records.size() >= kMostResults)
            break;
        const QJsonObject result = value.toObject();
        FoundRecord record = caseRecord(kCourtListener);
        Publication &p = record.publication;
        p.title = cleaned(result.value(QStringLiteral("caseName")).toString());
        if (p.title.isEmpty())
            continue;
        p.jurisdiction = QStringLiteral("United States");
        p.caseCitationForm = QStringLiteral("us");
        const QString courtId = result.value(QStringLiteral("court_id")).toString();
        p.court = bluebookCourt(courtId);
        if (p.court.isEmpty())
            p.court = cleaned(result.value(QStringLiteral("court")).toString());
        setDate(p, result.value(QStringLiteral("dateFiled")).toString());
        const QString docket = cleaned(result.value(QStringLiteral("docketNumber")).toString());
        if (!docket.isEmpty())
            p.caseNumber = docket.startsWith(QStringLiteral("No"), Qt::CaseInsensitive) ? docket : QStringLiteral("No. ") + docket;
        QStringList citations;
        for (const QJsonValue &citation : result.value(QStringLiteral("citation")).toArray())
            citations << cleaned(citation.toString());
        static const QRegularExpression parts(QStringLiteral("^(\\d{1,5})\\s+(.+?)\\s+(\\d{1,5})$"));
        if (const QRegularExpressionMatch match = parts.match(preferredCitation(citations)); match.hasMatch()) {
            p.volume = match.captured(1);
            p.containerTitle = match.captured(2);
            p.pages = match.captured(3);
        }
        p.url = sitePage(QStringLiteral("https://www.courtlistener.com"), result.value(QStringLiteral("absolute_url")).toString());
        records << record;
    }
    return records;
}

// ---- EUR-Lex

QString euEcli(const QString &text)
{
    static const QRegularExpression ecli(QStringLiteral("\\bECLI:EU:([CTF]):(\\d{4}):(\\d{1,6})\\b"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = ecli.match(text.simplified());
    if (!match.hasMatch())
        return QString();
    return QStringLiteral("ECLI:EU:%1:%2:%3").arg(match.captured(1).toUpper(), match.captured(2), match.captured(3));
}

QString celexForCaseNumber(const QString &caseNumber)
{
    const QString number = euCaseNumber(caseNumber);
    if (number.isEmpty())
        return QString();
    const QString court = number.left(1) == QLatin1StringView("C") ? QStringLiteral("CJ") : QStringLiteral("TJ");
    const int twoDigits = number.section(u'/', 1).toInt();
    const int year = twoDigits >= 54 ? 1900 + twoDigits : 2000 + twoDigits; // The Court first sat in 1954.
    const int serial = number.mid(2).section(u'/', 0, 0).toInt();
    return QStringLiteral("6%1%2%3").arg(year).arg(court).arg(serial, 4, 10, QLatin1Char('0'));
}

QUrl eurLexQueryUrl(const ProviderUrls &urls, const QString &ecli, const QString &celex)
{
    // Only values in a form already checked go into the query, so nothing in a case's details can change what it asks.
    static const QRegularExpression celexForm(QStringLiteral("^6\\d{4}[A-Z]{2}\\d{4}$"));
    QString match;
    if (!euEcli(ecli).isEmpty() && euEcli(ecli) == ecli)
        match = QStringLiteral("?work cdm:case-law_ecli ?ecli . FILTER(str(?ecli) = \"%1\")").arg(ecli);
    else if (celexForm.match(celex).hasMatch())
        match = QStringLiteral("?work cdm:resource_legal_id_celex ?celex . FILTER(str(?celex) = \"%1\")").arg(celex);
    else
        return QUrl();

    const QString query = QStringLiteral(
        "PREFIX cdm: <http://publications.europa.eu/ontology/cdm#> "
        "SELECT ?ecli ?celex ?date ?title WHERE { %1 "
        "OPTIONAL { ?work cdm:case-law_ecli ?ecli } "
        "OPTIONAL { ?work cdm:resource_legal_id_celex ?celex } "
        "OPTIONAL { ?work cdm:date_creation_legacy ?date } "
        "OPTIONAL { ?expression cdm:expression_belongs_to_work ?work ; "
        "cdm:expression_uses_language <http://publications.europa.eu/resource/authority/language/ENG> ; "
        "cdm:expression_title ?title } } LIMIT 5").arg(match);
    QUrl url(urls.eurLex + QStringLiteral("/webapi/rdf/sparql"));
    QUrlQuery items;
    items.addQueryItem(QStringLiteral("query"), QString::fromLatin1(QUrl::toPercentEncoding(query)));
    items.addQueryItem(QStringLiteral("format"), QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("application/sparql-results+json"))));
    url.setQuery(items);
    return url;
}

QList<FoundRecord> parseEurLex(const QByteArray &body)
{
    const QJsonArray bindings =
        QJsonDocument::fromJson(body).object().value(QStringLiteral("results")).toObject().value(QStringLiteral("bindings")).toArray();
    FoundRecord record = caseRecord(kEurLex);
    Publication &p = record.publication;
    p.jurisdiction = QStringLiteral("European Union");
    p.caseCitationForm = QStringLiteral("uk");
    const auto value = [](const QJsonObject &binding, const char *name) {
        return binding.value(QLatin1StringView(name)).toObject().value(QStringLiteral("value")).toString();
    };
    for (const QJsonValue &item : bindings) {
        const QJsonObject binding = item.toObject();
        if (p.neutralCitation.isEmpty())
            p.neutralCitation = euEcli(value(binding, "ecli"));
        if (!p.year)
            setDate(p, value(binding, "date"));
        if (p.caseNumber.isEmpty()) {
            // Such as 62012CJ0131, for Case C-131/12.
            static const QRegularExpression celex(QStringLiteral("^6(\\d{4})([CT])[A-Z](\\d{4})$"));
            if (const QRegularExpressionMatch match = celex.match(value(binding, "celex")); match.hasMatch())
                p.caseNumber = QStringLiteral("%1-%2/%3").arg(match.captured(2)).arg(match.captured(3).toInt()).arg(match.captured(1).right(2));
        }
        // "Judgment of the Court (Grand Chamber), 13 May 2014.#Parties v Others.#What it's about.#Case C-131/12."
        const QStringList parts = value(binding, "title").split(u'#');
        if (p.title.isEmpty() && parts.size() >= 2) {
            QString name = cleaned(parts.at(1));
            if (name.endsWith(u'.'))
                name.chop(1);
            p.title = name;
            const QString heading = parts.at(0);
            if (heading.contains(QStringLiteral("General Court")))
                p.court = QStringLiteral("General Court");
            else if (heading.contains(QStringLiteral("of the Court")))
                p.court = QStringLiteral("Court of Justice");
        }
    }
    if (p.court.isEmpty() && !p.neutralCitation.isEmpty())
        p.court = p.neutralCitation.section(u':', 2, 2) == QLatin1StringView("C") ? QStringLiteral("Court of Justice") : QStringLiteral("General Court");
    if (p.title.isEmpty() && p.neutralCitation.isEmpty())
        return {};
    return {record};
}

// ---- CanLII

std::optional<CanLiiCase> canLiiCaseFor(const QString &neutralCitation)
{
    const QRegularExpressionMatch match = canadianCitation().match(neutralCitation.simplified());
    if (!match.hasMatch())
        return std::nullopt;
    const CanadianCourt *court = canadianCourt(match.captured(2));
    if (!court)
        return std::nullopt;
    return CanLiiCase{QString::fromLatin1(court->database),
                      (match.captured(1) + match.captured(2) + QString::number(match.captured(3).toInt())).toLower()};
}

QUrl canLiiCaseUrl(const ProviderUrls &urls, const CanLiiCase &which)
{
    static const QRegularExpression safe(QStringLiteral("^[a-z0-9-]{2,20}$"));
    if (!safe.match(which.databaseId).hasMatch() || !safe.match(which.caseId).hasMatch())
        return QUrl();
    return QUrl(urls.canLii + QStringLiteral("/v1/caseBrowse/en/%1/%2/").arg(which.databaseId, which.caseId));
}

std::optional<FoundRecord> parseCanLii(const QByteArray &body)
{
    const QJsonObject result = QJsonDocument::fromJson(body).object();
    FoundRecord record = caseRecord(kCanLii);
    Publication &p = record.publication;
    p.title = cleaned(result.value(QStringLiteral("title")).toString());
    const QString citation = cleaned(result.value(QStringLiteral("citation")).toString()).remove(QStringLiteral(" (CanLII)"));
    const QRegularExpressionMatch match = canadianCitation().match(citation);
    if (p.title.isEmpty() || !match.hasMatch())
        return std::nullopt;
    p.neutralCitation = citation;
    if (const CanadianCourt *court = canadianCourt(match.captured(2)))
        p.court = QString::fromLatin1(court->court);
    p.jurisdiction = QStringLiteral("Canada");
    p.caseCitationForm = QStringLiteral("uk");
    setDate(p, result.value(QStringLiteral("decisionDate")).toString());
    p.caseNumber = cleaned(result.value(QStringLiteral("docketNumber")).toString());
    p.url = sitePage(QStringLiteral("https://canlii.ca"), result.value(QStringLiteral("url")).toString());
    return record;
}

// ---- Matching

double caseNameSimilarity(const QString &a, const QString &b)
{
    return titleSimilarity(caseNameKey(a), caseNameKey(b));
}

double caseMatchScore(const Publication &publication, const FoundRecord &found)
{
    const double name = caseNameSimilarity(publication.title, found.publication.title);
    if (name < 0.5)
        return name * 0.8; // Nothing else rescues a different name.
    double total = name;
    double weight = 1.0;
    if (publication.year && found.publication.year) {
        const int apart = qAbs(*publication.year - *found.publication.year);
        total += apart == 0 ? 0.3 : apart == 1 ? 0.15 : 0.0;
        weight += 0.3;
    }
    if (!publication.court.isEmpty() && !found.publication.court.isEmpty()) {
        const QString mine = foldedText(publication.court);
        const QString theirs = foldedText(found.publication.court);
        total += mine.contains(theirs) || theirs.contains(mine) ? 0.2 : 0.0;
        weight += 0.2;
    }
    return total / weight;
}

QString sameCaseCitation(const Publication &publication, const FoundRecord &found)
{
    const Publication &theirs = found.publication;
    if (!publication.neutralCitation.isEmpty() && !theirs.neutralCitation.isEmpty()
        && normalisedCitation(publication.neutralCitation) == normalisedCitation(theirs.neutralCitation)) {
        return euEcli(publication.neutralCitation).isEmpty() ? QStringLiteral("neutral_citation") : QStringLiteral("ecli");
    }
    const QString mine = euCaseNumber(publication.caseNumber);
    if (!mine.isEmpty() && mine == euCaseNumber(theirs.caseNumber))
        return QStringLiteral("case_number");
    const QString report = reporterCitationOf(publication);
    if (!report.isEmpty() && report.simplified().compare(reporterCitationOf(theirs).simplified(), Qt::CaseInsensitive) == 0)
        return QStringLiteral("report");
    return QString();
}

bool caseNameAgrees(const Publication &publication, const QStringList &fileNames, const FoundRecord &found)
{
    constexpr double kAgrees = 0.5;
    if (publication.title.trimmed().isEmpty() || caseNameSimilarity(publication.title, found.publication.title) >= kAgrees)
        return true;
    return std::any_of(fileNames.cbegin(), fileNames.cend(), [&](const QString &name) {
        return caseNameSimilarity(QFileInfo(name).completeBaseName(), found.publication.title) >= kAgrees;
    });
}

} // namespace papertrail::metadata
