#include "metadata/Providers.h"

#include "core/model/Identifiers.h"
#include "core/model/PublicationFields.h"
#include "import/Extractors.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace papertrail::metadata {

using namespace core;

const QString kCrossref = QStringLiteral("crossref");
const QString kOpenAlex = QStringLiteral("openalex");
const QString kArxiv = QStringLiteral("arxiv");
const QString kOpenLibrary = QStringLiteral("openlibrary");
const QString kBibTeXImport = QStringLiteral("bibtex");
const QString kRisImport = QStringLiteral("ris");

namespace {

// Crossref's and OpenAlex's text can hold markup, such as <i> in a title, or JATS in an abstract.
QString cleanText(const QString &text)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]{1,200}>"));
    QString cleaned = text;
    cleaned.replace(tags, QStringLiteral(" "));
    return import::decodeHtmlEntities(cleaned).simplified();
}

QString firstString(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
        const QString text = cleanText(item.toString());
        if (!text.isEmpty())
            return text;
    }
    return {};
}

QJsonDocument parsedJson(const QByteArray &body)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    return error.error == QJsonParseError::NoError ? document : QJsonDocument();
}

// Years outside this are mistakes, such as a page number in the wrong field.
std::optional<int> sensibleYear(int year)
{
    return year >= 1000 && year <= 2200 ? std::optional<int>(year) : std::nullopt;
}

void setDateFromParts(Publication &p, const QJsonValue &date)
{
    const QJsonArray parts = date.toObject().value(QStringLiteral("date-parts")).toArray().at(0).toArray();
    if (parts.isEmpty())
        return;
    p.year = sensibleYear(parts.at(0).toInt());
    if (p.year && parts.size() > 1 && parts.at(1).toInt() >= 1 && parts.at(1).toInt() <= 12) {
        p.month = parts.at(1).toInt();
        if (parts.size() > 2 && parts.at(2).toInt() >= 1 && parts.at(2).toInt() <= 31)
            p.day = parts.at(2).toInt();
    }
}

QList<PersonName> crossrefPeople(const QJsonValue &value)
{
    QList<PersonName> people;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
        const QJsonObject person = item.toObject();
        PersonName name;
        name.family = cleanText(person.value(QStringLiteral("family")).toString());
        name.given = cleanText(person.value(QStringLiteral("given")).toString());
        if (name.family.isEmpty()) // An organisation, which goes under its whole name.
            name.family = cleanText(person.value(QStringLiteral("name")).toString());
        if (!name.family.isEmpty())
            people << name;
    }
    return people;
}

std::optional<PublicationType> typeFromCrossref(const QString &type)
{
    static const QMap<QString, PublicationType> types = {
        {QStringLiteral("journal-article"), PublicationType::JournalArticle},
        {QStringLiteral("journal-volume"), PublicationType::JournalIssue},
        {QStringLiteral("journal-issue"), PublicationType::JournalIssue},
        {QStringLiteral("proceedings-article"), PublicationType::ConferencePaper},
        {QStringLiteral("proceedings"), PublicationType::ConferenceProceedings},
        {QStringLiteral("book"), PublicationType::Book},
        {QStringLiteral("monograph"), PublicationType::Book},
        {QStringLiteral("edited-book"), PublicationType::Book},
        {QStringLiteral("reference-book"), PublicationType::Book},
        {QStringLiteral("book-chapter"), PublicationType::BookChapter},
        {QStringLiteral("book-section"), PublicationType::BookChapter},
        {QStringLiteral("dissertation"), PublicationType::Dissertation},
        {QStringLiteral("report"), PublicationType::TechnicalReport},
        {QStringLiteral("report-series"), PublicationType::TechnicalReport},
        {QStringLiteral("standard"), PublicationType::Standard},
    };
    const auto found = types.constFind(type);
    return found == types.constEnd() ? std::nullopt : std::optional<PublicationType>(*found);
}

FoundRecord recordFromCrossref(const QJsonObject &work)
{
    FoundRecord record;
    record.provider = kCrossref;
    Publication &p = record.publication;

    p.title = firstString(work.value(QStringLiteral("title")));
    p.subtitle = firstString(work.value(QStringLiteral("subtitle")));
    p.doi = normaliseDoi(work.value(QStringLiteral("DOI")).toString());
    p.authors = crossrefPeople(work.value(QStringLiteral("author")));
    p.editors = crossrefPeople(work.value(QStringLiteral("editor")));
    p.containerTitle = firstString(work.value(QStringLiteral("container-title")));
    p.volume = cleanText(work.value(QStringLiteral("volume")).toString());
    p.issue = cleanText(work.value(QStringLiteral("issue")).toString());
    p.pages = cleanText(work.value(QStringLiteral("page")).toString());
    p.publisher = cleanText(work.value(QStringLiteral("publisher")).toString());
    p.language = cleanText(work.value(QStringLiteral("language")).toString());
    p.abstract = cleanText(work.value(QStringLiteral("abstract")).toString());

    // "issued" is the earliest date, and is what a citation generally uses.
    setDateFromParts(p, work.value(QStringLiteral("issued")));
    if (!p.year)
        setDateFromParts(p, work.value(QStringLiteral("published")));

    const QJsonArray issnTypes = work.value(QStringLiteral("issn-type")).toArray();
    for (const QJsonValue &item : issnTypes) {
        const QJsonObject issn = item.toObject();
        if (issn.value(QStringLiteral("type")).toString() == QLatin1StringView("electronic"))
            p.eissn = normaliseIssn(issn.value(QStringLiteral("value")).toString());
        else
            p.issn = normaliseIssn(issn.value(QStringLiteral("value")).toString());
    }
    if (p.issn.isEmpty() && p.eissn.isEmpty())
        p.issn = normaliseIssn(firstString(work.value(QStringLiteral("ISSN"))));

    const QJsonArray isbns = work.value(QStringLiteral("ISBN")).toArray();
    for (const QJsonValue &item : isbns) {
        const QString isbn = normaliseIsbn(item.toString());
        if (isbn.size() == 13 && p.isbn13.isEmpty())
            p.isbn13 = isbn;
        else if (isbn.size() == 10 && p.isbn10.isEmpty())
            p.isbn10 = isbn;
    }

    const QJsonObject event = work.value(QStringLiteral("event")).toObject();
    p.conferenceName = cleanText(event.value(QStringLiteral("name")).toString());
    p.conferenceLocation = cleanText(event.value(QStringLiteral("location")).toString());
    p.institution = cleanText(work.value(QStringLiteral("institution")).toArray().at(0).toObject().value(QStringLiteral("name")).toString());

    const QString type = work.value(QStringLiteral("type")).toString();
    record.type = typeFromCrossref(type);
    record.preprintHint = type == QLatin1StringView("posted-content")
        && work.value(QStringLiteral("subtype")).toString(QStringLiteral("preprint")) == QLatin1StringView("preprint");
    return record;
}

std::optional<PublicationType> typeFromOpenAlex(const QString &type, const QString &sourceType)
{
    if (type == QLatin1StringView("article") || type == QLatin1StringView("review")) {
        // OpenAlex calls most things articles, so this only goes by what the article is in.
        if (sourceType == QLatin1StringView("journal"))
            return PublicationType::JournalArticle;
        if (sourceType == QLatin1StringView("conference"))
            return PublicationType::ConferencePaper;
        return std::nullopt;
    }
    if (type == QLatin1StringView("book"))
        return PublicationType::Book;
    if (type == QLatin1StringView("book-chapter"))
        return PublicationType::BookChapter;
    if (type == QLatin1StringView("dissertation"))
        return PublicationType::Dissertation;
    if (type == QLatin1StringView("report"))
        return PublicationType::TechnicalReport;
    if (type == QLatin1StringView("standard"))
        return PublicationType::Standard;
    return std::nullopt;
}

FoundRecord recordFromOpenAlex(const QJsonObject &work)
{
    FoundRecord record;
    record.provider = kOpenAlex;
    Publication &p = record.publication;

    p.title = cleanText(work.value(QStringLiteral("title")).toString());
    p.doi = normaliseDoi(work.value(QStringLiteral("doi")).toString());
    p.year = sensibleYear(work.value(QStringLiteral("publication_year")).toInt());
    p.language = cleanText(work.value(QStringLiteral("language")).toString());

    const QJsonArray authorships = work.value(QStringLiteral("authorships")).toArray();
    for (const QJsonValue &item : authorships) {
        const QString name = cleanText(item.toObject().value(QStringLiteral("author")).toObject().value(QStringLiteral("display_name")).toString());
        if (!name.isEmpty())
            p.authors << import::parsePersonName(name);
    }

    const QJsonObject source = work.value(QStringLiteral("primary_location")).toObject().value(QStringLiteral("source")).toObject();
    p.containerTitle = cleanText(source.value(QStringLiteral("display_name")).toString());
    p.issn = normaliseIssn(source.value(QStringLiteral("issn_l")).toString());

    const QJsonObject biblio = work.value(QStringLiteral("biblio")).toObject();
    p.volume = cleanText(biblio.value(QStringLiteral("volume")).toString());
    p.issue = cleanText(biblio.value(QStringLiteral("issue")).toString());
    const QString first = cleanText(biblio.value(QStringLiteral("first_page")).toString());
    const QString last = cleanText(biblio.value(QStringLiteral("last_page")).toString());
    p.pages = !first.isEmpty() && !last.isEmpty() && first != last ? first + u'-' + last : first;

    const QString pmid = work.value(QStringLiteral("ids")).toObject().value(QStringLiteral("pmid")).toString();
    p.pmid = pmid.section(u'/', -1);

    // The abstract comes as a list of where each word goes, because of how they licensed them.
    const QJsonObject words = work.value(QStringLiteral("abstract_inverted_index")).toObject();
    QMap<int, QString> ordered;
    for (auto it = words.begin(); it != words.end(); ++it) {
        const QJsonArray positions = it.value().toArray();
        for (const QJsonValue &position : positions)
            ordered.insert(position.toInt(), it.key());
    }
    p.abstract = cleanText(QStringList(ordered.cbegin(), ordered.cend()).join(u' '));

    const QString type = work.value(QStringLiteral("type")).toString();
    record.type = typeFromOpenAlex(type, source.value(QStringLiteral("type")).toString());
    record.preprintHint = type == QLatin1StringView("preprint");
    return record;
}

} // namespace

QJsonObject FoundRecord::toJson() const
{
    QJsonObject fields;
    for (const QString &name : metadataFieldNames()) {
        if (name == QLatin1StringView("type"))
            continue;
        const QString value = metadataFieldValue(publication, name);
        if (!value.isEmpty())
            fields.insert(name, value);
    }
    QJsonObject json{{QStringLiteral("fields"), fields},
                     {QStringLiteral("provider"), provider},
                     {QStringLiteral("matchedBy"), matchedBy},
                     {QStringLiteral("score"), score},
                     {QStringLiteral("preprintHint"), preprintHint}};
    if (type)
        json.insert(QStringLiteral("type"), toDbString(*type));
    return json;
}

FoundRecord FoundRecord::fromJson(const QJsonObject &json)
{
    FoundRecord record;
    const QJsonObject fields = json.value(QStringLiteral("fields")).toObject();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        setMetadataFieldValue(record.publication, it.key(), it.value().toString());
    record.provider = json.value(QStringLiteral("provider")).toString();
    record.matchedBy = json.value(QStringLiteral("matchedBy")).toString();
    record.score = json.value(QStringLiteral("score")).toDouble();
    record.preprintHint = json.value(QStringLiteral("preprintHint")).toBool();
    if (json.contains(QStringLiteral("type")))
        record.type = publicationTypeFromDb(json.value(QStringLiteral("type")).toString());
    return record;
}

QUrl crossrefWorkUrl(const ProviderUrls &urls, const QString &doi)
{
    return QUrl(urls.crossref + QStringLiteral("/works/") + QString::fromLatin1(QUrl::toPercentEncoding(doi)), QUrl::TolerantMode);
}

QUrl crossrefNoticesUrl(const ProviderUrls &urls, const QStringList &dois)
{
    QUrl url(urls.crossref + QStringLiteral("/works"));
    QStringList filters;
    for (const QString &doi : dois)
        filters << QStringLiteral("updates:") + QString::fromLatin1(QUrl::toPercentEncoding(doi.trimmed().toLower(), "/()"));
    QUrlQuery query;
    // Repeated, they're each matched: notices about any of these works.
    query.addQueryItem(QStringLiteral("filter"), filters.join(u','));
    query.addQueryItem(QStringLiteral("rows"), QStringLiteral("1000"));
    query.addQueryItem(QStringLiteral("select"), QStringLiteral("DOI,update-to"));
    url.setQuery(query);
    return url;
}

QHash<QString, QList<core::Notice>> parseCrossrefNotices(const QByteArray &body, bool *readable)
{
    QHash<QString, QList<core::Notice>> found;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    const QJsonObject message = document.object().value(QStringLiteral("message")).toObject();
    const bool ok = error.error == QJsonParseError::NoError && message.value(QStringLiteral("items")).isArray();
    if (readable)
        *readable = ok;
    if (!ok)
        return found;
    static const QRegularExpression kind(QStringLiteral("^[a-z][a-z_-]{0,40}$"));
    for (const QJsonValue &itemValue : message.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = itemValue.toObject();
        const QString noticeDoi = normaliseDoi(item.value(QStringLiteral("DOI")).toString());
        if (noticeDoi.isEmpty())
            continue;
        for (const QJsonValue &updateValue : item.value(QStringLiteral("update-to")).toArray()) {
            const QJsonObject update = updateValue.toObject();
            const QString target = normaliseDoi(update.value(QStringLiteral("DOI")).toString()).toLower();
            core::Notice notice;
            notice.kind = update.value(QStringLiteral("type")).toString().trimmed().toLower().replace(u'-', u'_');
            if (target.isEmpty() || !kind.match(notice.kind).hasMatch())
                continue;
            notice.noticeDoi = noticeDoi;
            notice.label = update.value(QStringLiteral("label")).toString().simplified().left(80);
            if (notice.label.isEmpty()) {
                notice.label = notice.kind;
                notice.label.replace(u'_', u' ');
                if (!notice.label.isEmpty())
                    notice.label[0] = notice.label.at(0).toUpper();
            }
            notice.source = update.value(QStringLiteral("source")).toString().simplified().left(40);
            const QJsonArray parts = update.value(QStringLiteral("updated")).toObject().value(QStringLiteral("date-parts")).toArray().at(0).toArray();
            QStringList date;
            for (int i = 0; i < parts.size() && i < 3; ++i) {
                const int number = parts.at(i).toInt();
                if (number <= 0)
                    break;
                date << (i == 0 ? QString::number(number) : QStringLiteral("%1").arg(number, 2, 10, QLatin1Char('0')));
            }
            notice.date = date.join(u'-');
            // The same notice can be listed twice, from the publisher and from Retraction Watch.
            QList<core::Notice> &list = found[target];
            const bool known = std::any_of(list.cbegin(), list.cend(), [&](const core::Notice &other) {
                return other.noticeDoi == notice.noticeDoi && other.kind == notice.kind;
            });
            if (!known)
                list << notice;
        }
    }
    return found;
}

QUrl crossrefSearchUrl(const ProviderUrls &urls, const QString &title)
{
    QUrl url(urls.crossref + QStringLiteral("/works"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query.bibliographic"), QString::fromLatin1(QUrl::toPercentEncoding(title)));
    query.addQueryItem(QStringLiteral("rows"), QStringLiteral("10"));
    // Without this, reviews of a paper and data sets about it come ahead of the paper.
    query.addQueryItem(QStringLiteral("filter"),
                       QStringLiteral("type:journal-article,type:proceedings-article,type:book,type:book-chapter,"
                                      "type:monograph,type:edited-book,type:reference-book,type:dissertation,"
                                      "type:report,type:posted-content,type:standard"));
    url.setQuery(query);
    return url;
}

QUrl openAlexWorkUrl(const ProviderUrls &urls, const QString &doi)
{
    return QUrl(urls.openAlex + QStringLiteral("/works/doi:") + QString::fromLatin1(QUrl::toPercentEncoding(doi, "/")), QUrl::TolerantMode);
}

QUrl openAlexSearchUrl(const ProviderUrls &urls, const QString &title)
{
    QUrl url(urls.openAlex + QStringLiteral("/works"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("search"), QString::fromLatin1(QUrl::toPercentEncoding(title)));
    query.addQueryItem(QStringLiteral("per-page"), QStringLiteral("10"));
    url.setQuery(query);
    return url;
}

QUrl arxivUrl(const ProviderUrls &urls, const QString &arxivId)
{
    QUrl url(urls.arxiv + QStringLiteral("/api/query"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id_list"), QString::fromLatin1(QUrl::toPercentEncoding(arxivId, "/")));
    url.setQuery(query);
    return url;
}

QUrl openLibraryUrl(const ProviderUrls &urls, const QString &isbn)
{
    QUrl url(urls.openLibrary + QStringLiteral("/api/books"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("bibkeys"), QStringLiteral("ISBN:") + QString::fromLatin1(QUrl::toPercentEncoding(isbn)));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("jscmd"), QStringLiteral("data"));
    url.setQuery(query);
    return url;
}

std::optional<FoundRecord> parseCrossrefWork(const QByteArray &body)
{
    const QJsonObject message = parsedJson(body).object().value(QStringLiteral("message")).toObject();
    if (message.isEmpty())
        return std::nullopt;
    FoundRecord record = recordFromCrossref(message);
    return record.publication.title.isEmpty() ? std::nullopt : std::optional<FoundRecord>(record);
}

QList<FoundRecord> parseCrossrefSearch(const QByteArray &body)
{
    QList<FoundRecord> records;
    const QJsonArray items = parsedJson(body).object().value(QStringLiteral("message")).toObject().value(QStringLiteral("items")).toArray();
    for (const QJsonValue &item : items) {
        FoundRecord record = recordFromCrossref(item.toObject());
        if (!record.publication.title.isEmpty())
            records << record;
    }
    return records;
}

std::optional<FoundRecord> parseOpenAlexWork(const QByteArray &body)
{
    const QJsonObject work = parsedJson(body).object();
    if (work.isEmpty())
        return std::nullopt;
    FoundRecord record = recordFromOpenAlex(work);
    return record.publication.title.isEmpty() ? std::nullopt : std::optional<FoundRecord>(record);
}

QList<FoundRecord> parseOpenAlexSearch(const QByteArray &body)
{
    QList<FoundRecord> records;
    const QJsonArray items = parsedJson(body).object().value(QStringLiteral("results")).toArray();
    for (const QJsonValue &item : items) {
        FoundRecord record = recordFromOpenAlex(item.toObject());
        if (!record.publication.title.isEmpty())
            records << record;
    }
    return records;
}

std::optional<FoundRecord> parseArxiv(const QByteArray &body)
{
    FoundRecord record;
    record.provider = kArxiv;
    Publication &p = record.publication;
    QXmlStreamReader reader(body);
    bool inEntry = false;
    bool inAuthor = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name();
            if (name == QLatin1StringView("entry")) {
                inEntry = true;
            } else if (!inEntry) {
                continue;
            } else if (name == QLatin1StringView("author")) {
                inAuthor = true;
            } else if (inAuthor && name == QLatin1StringView("name")) {
                const QString author = cleanText(reader.readElementText());
                if (!author.isEmpty())
                    p.authors << import::parsePersonName(author);
            } else if (name == QLatin1StringView("title")) {
                p.title = cleanText(reader.readElementText());
            } else if (name == QLatin1StringView("summary")) {
                p.abstract = cleanText(reader.readElementText());
            } else if (name == QLatin1StringView("published")) {
                const QString date = reader.readElementText();
                p.year = sensibleYear(date.left(4).toInt());
                if (p.year && date.size() >= 10) {
                    p.month = date.mid(5, 2).toInt();
                    p.day = date.mid(8, 2).toInt();
                }
            } else if (name == QLatin1StringView("doi")) {
                p.doi = normaliseDoi(reader.readElementText());
            } else if (name == QLatin1StringView("id")) {
                static const QRegularExpression id(QStringLiteral("abs/(.+?)(v\\d+)?$"));
                const auto match = id.match(reader.readElementText());
                if (match.hasMatch())
                    p.arxivId = normaliseArxivId(match.captured(1));
            }
        } else if (reader.isEndElement()) {
            if (reader.name() == QLatin1StringView("author"))
                inAuthor = false;
            else if (reader.name() == QLatin1StringView("entry"))
                break; // Only ever one, when asking by id.
        }
    }
    if (reader.hasError() || !inEntry || p.title.isEmpty())
        return std::nullopt;
    p.url = QStringLiteral("https://arxiv.org/abs/") + p.arxivId;
    // Unless it says a journal has published it since, this is a preprint.
    record.preprintHint = p.doi.isEmpty();
    return record;
}

std::optional<FoundRecord> parseOpenLibrary(const QByteArray &body)
{
    const QJsonObject all = parsedJson(body).object();
    if (all.isEmpty())
        return std::nullopt;
    const QJsonObject book = all.begin().value().toObject();

    FoundRecord record;
    record.provider = kOpenLibrary;
    record.type = PublicationType::Book;
    Publication &p = record.publication;
    p.title = cleanText(book.value(QStringLiteral("title")).toString());
    p.subtitle = cleanText(book.value(QStringLiteral("subtitle")).toString());
    if (p.title.isEmpty())
        return std::nullopt;

    const QJsonArray authors = book.value(QStringLiteral("authors")).toArray();
    for (const QJsonValue &author : authors) {
        const QString name = cleanText(author.toObject().value(QStringLiteral("name")).toString());
        if (!name.isEmpty())
            p.authors << import::parsePersonName(name);
    }
    p.publisher = cleanText(book.value(QStringLiteral("publishers")).toArray().at(0).toObject().value(QStringLiteral("name")).toString());
    p.place = cleanText(book.value(QStringLiteral("publish_places")).toArray().at(0).toObject().value(QStringLiteral("name")).toString());

    // A publish date can be "2009", "March 2009" or "9/1/2009".
    static const QRegularExpression year(QStringLiteral("\\b(1[0-9]{3}|20[0-9]{2})\\b"));
    const auto match = year.match(book.value(QStringLiteral("publish_date")).toString());
    if (match.hasMatch())
        p.year = sensibleYear(match.captured(1).toInt());

    const QJsonObject identifiers = book.value(QStringLiteral("identifiers")).toObject();
    p.isbn13 = normaliseIsbn(identifiers.value(QStringLiteral("isbn_13")).toArray().at(0).toString());
    p.isbn10 = normaliseIsbn(identifiers.value(QStringLiteral("isbn_10")).toArray().at(0).toString());
    return record;
}

} // namespace papertrail::metadata
