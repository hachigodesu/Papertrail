#include "metadata/SelectionLookup.h"

#include "metadata/Transport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>

namespace papertrail::metadata {

namespace {

// Long enough for any title, short enough that nothing odd in an answer goes back out at length.
constexpr int kMaxKeyLength = 300;

QByteArray encoded(const QString &part)
{
    return QUrl::toPercentEncoding(part);
}

QUrl strictUrl(const QByteArray &address)
{
    const QUrl url = QUrl::fromEncoded(address, QUrl::StrictMode);
    return url.isValid() ? url : QUrl();
}

QString keyFor(const QString &words)
{
    QString key = words.simplified();
    key.replace(u' ', u'_');
    return key;
}

QString shortened(QString text, int maxChars)
{
    text = text.simplified();
    if (text.size() > maxChars)
        text.truncate(maxChars);
    return text;
}

} // namespace

QString lookupWords(const QString &selection, int maxWords, int maxChars)
{
    QString text = selection;
    // Words broken across lines come back together; quotes would only confuse a search.
    static const QRegularExpression brokenWord(QStringLiteral("(\\w)-\\s*\\n\\s*(\\w)"),
                                               QRegularExpression::UseUnicodePropertiesOption);
    text.replace(brokenWord, QStringLiteral("\\1\\2"));
    for (QChar &c : text) {
        if (c == u'"' || c == u'“' || c == u'”' || c.category() == QChar::Other_Control
            || c.category() == QChar::Other_Format || c.category() == QChar::Other_PrivateUse)
            c = u' ';
    }
    QStringList words = text.simplified().split(u' ', Qt::SkipEmptyParts);
    if (words.size() > maxWords)
        words = words.mid(0, maxWords);
    QString joined = words.join(u' ');
    if (joined.size() > maxChars) {
        joined.truncate(maxChars);
        const qsizetype space = joined.lastIndexOf(u' ');
        if (space > 0)
            joined.truncate(space);
    }
    // Punctuation at either end, such as a full stop or brackets, isn't part of what's being looked up.
    auto isEdge = [](QChar c) { return c.isPunct() && c != u'-' && c != u'\''; };
    while (!joined.isEmpty() && isEdge(joined.front()))
        joined.remove(0, 1);
    while (!joined.isEmpty() && isEdge(joined.back()))
        joined.chop(1);
    return joined.trimmed();
}

int wordCount(const QString &words)
{
    return int(words.split(u' ', Qt::SkipEmptyParts).size());
}

bool isWikiLanguage(const QString &code)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z]{2,3}$"));
    return pattern.match(code).hasMatch();
}

QString wikiLanguageFor(const QString &localeName)
{
    const QString language = localeName.section(u'_', 0, 0).section(u'-', 0, 0).toLower();
    return isWikiLanguage(language) && language != QLatin1StringView("c") ? language : QStringLiteral("en");
}

QUrl wikipediaSearchUrl(const QString &language, const QString &words, int limit)
{
    if (!isWikiLanguage(language) || words.isEmpty())
        return {};
    return strictUrl("https://" + language.toLatin1() + ".wikipedia.org/w/rest.php/v1/search/page?q=" + encoded(words)
                     + "&limit=" + QByteArray::number(qBound(1, limit, 10)));
}

QUrl wikipediaSummaryUrl(const QString &language, const QString &key)
{
    if (!isWikiLanguage(language) || key.isEmpty() || key.size() > kMaxKeyLength)
        return {};
    return strictUrl("https://" + language.toLatin1() + ".wikipedia.org/api/rest_v1/page/summary/" + encoded(keyFor(key)));
}

QUrl wikipediaPageLink(const QString &language, const QString &key)
{
    if (!isWikiLanguage(language) || key.isEmpty() || key.size() > kMaxKeyLength)
        return {};
    return strictUrl("https://" + language.toLatin1() + ".wikipedia.org/wiki/" + encoded(keyFor(key)));
}

QUrl wiktionaryDefinitionUrl(const QString &term)
{
    if (term.isEmpty() || term.size() > kMaxKeyLength)
        return {};
    return strictUrl("https://en.wiktionary.org/api/rest_v1/page/definition/" + encoded(keyFor(term)));
}

QUrl wiktionaryPageLink(const QString &term)
{
    if (term.isEmpty() || term.size() > kMaxKeyLength)
        return {};
    return strictUrl("https://en.wiktionary.org/wiki/" + encoded(keyFor(term)));
}

QString plainFromHtml(const QString &html)
{
    QString text;
    text.reserve(html.size());
    bool inTag = false;
    for (QChar c : html) {
        if (inTag) {
            if (c == u'>')
                inTag = false;
        } else if (c == u'<') {
            inTag = true;
        } else {
            text += c;
        }
    }
    static const QRegularExpression reference(QStringLiteral("&(#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[a-zA-Z]{2,8});"));
    QString out;
    qsizetype from = 0;
    for (auto match = reference.globalMatch(text); match.hasNext();) {
        const QRegularExpressionMatch m = match.next();
        out += QStringView(text).mid(from, m.capturedStart() - from);
        const QString name = m.captured(1);
        char32_t code = 0;
        if (name.startsWith(QLatin1StringView("#x")) || name.startsWith(QLatin1StringView("#X")))
            code = name.mid(2).toUInt(nullptr, 16);
        else if (name.startsWith(u'#'))
            code = name.mid(1).toUInt();
        else if (name == QLatin1StringView("amp"))
            code = u'&';
        else if (name == QLatin1StringView("lt"))
            code = u'<';
        else if (name == QLatin1StringView("gt"))
            code = u'>';
        else if (name == QLatin1StringView("quot"))
            code = u'"';
        else if (name == QLatin1StringView("apos"))
            code = u'\'';
        else if (name == QLatin1StringView("nbsp"))
            code = u' ';
        // Only characters that can be shown: nothing that controls text or isn't a character at all.
        if (code >= 0x20 && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF) && code != 0x7F
            && !(code >= 0x80 && code < 0xA0))
            out += QString::fromUcs4(&code, 1);
        else
            out += m.captured(0);
        from = m.capturedEnd();
    }
    out += QStringView(text).mid(from);
    return out.simplified();
}

QList<WikipediaPage> parseWikipediaSearch(const QByteArray &body)
{
    QList<WikipediaPage> pages;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    for (const QJsonValue &value : document.object().value(QStringLiteral("pages")).toArray()) {
        const QJsonObject page = value.toObject();
        WikipediaPage found;
        found.key = page.value(QStringLiteral("key")).toString();
        found.title = shortened(page.value(QStringLiteral("title")).toString(), kMaxKeyLength);
        found.description = shortened(page.value(QStringLiteral("description")).toString(), 300);
        if (found.key.isEmpty() || found.key.size() > kMaxKeyLength)
            continue;
        if (found.title.isEmpty())
            found.title = shortened(QString(found.key).replace(u'_', u' '), kMaxKeyLength);
        pages << found;
    }
    return pages;
}

std::optional<WikipediaSummary> parseWikipediaSummary(const QByteArray &body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return std::nullopt;
    const QJsonObject page = document.object();
    WikipediaSummary summary;
    summary.title = shortened(page.value(QStringLiteral("title")).toString(), kMaxKeyLength);
    summary.description = shortened(page.value(QStringLiteral("description")).toString(), 300);
    summary.extract = page.value(QStringLiteral("extract")).toString().trimmed();
    if (summary.extract.size() > 4000)
        summary.extract = summary.extract.left(4000) + QStringLiteral("...");
    summary.disambiguation = page.value(QStringLiteral("type")).toString() == QLatin1StringView("disambiguation");
    summary.key = page.value(QStringLiteral("titles")).toObject().value(QStringLiteral("canonical")).toString();
    if (summary.key.isEmpty() || summary.key.size() > kMaxKeyLength)
        summary.key = keyFor(summary.title);
    if (summary.title.isEmpty() || (summary.extract.isEmpty() && !summary.disambiguation))
        return std::nullopt;
    return summary;
}

QList<WiktionaryEntry> parseWiktionary(const QByteArray &body, const QString &languageFirst)
{
    QList<WiktionaryEntry> first;
    QList<WiktionaryEntry> rest;
    const QJsonObject languages = QJsonDocument::fromJson(body).object();
    for (auto language = languages.begin(); language != languages.end(); ++language) {
        if (!language.value().isArray())
            continue;
        for (const QJsonValue &value : language.value().toArray()) {
            const QJsonObject part = value.toObject();
            WiktionaryEntry entry;
            entry.languageCode = shortened(language.key(), 20);
            entry.language = shortened(part.value(QStringLiteral("language")).toString(), 60);
            entry.partOfSpeech = shortened(part.value(QStringLiteral("partOfSpeech")).toString(), 60);
            for (const QJsonValue &definition : part.value(QStringLiteral("definitions")).toArray()) {
                const QString text = plainFromHtml(definition.toObject().value(QStringLiteral("definition")).toString());
                if (!text.isEmpty() && entry.definitions.size() < 12)
                    entry.definitions << shortened(text, 1000);
            }
            if (entry.definitions.isEmpty())
                continue;
            if (entry.language.isEmpty())
                entry.language = entry.languageCode;
            (entry.languageCode == languageFirst ? first : rest) << entry;
        }
    }
    return first + rest;
}

SelectionLookup::SelectionLookup(Transport &transport, QObject *parent) : QObject(parent), m_transport(transport) {}

void SelectionLookup::cancel()
{
    ++m_wikipediaGeneration;
    ++m_wiktionaryGeneration;
}

QString SelectionLookup::problemWith(int status, const QString &error) const
{
    if (status == 429)
        return tr("It's had too many requests for now. Try again in a while.");
    if (!error.isEmpty())
        return error;
    return tr("It answered with an error (%1).").arg(status);
}

void SelectionLookup::lookUpOnWikipedia(const QString &words, const QString &language)
{
    const int generation = ++m_wikipediaGeneration;
    const QUrl url = wikipediaSearchUrl(language, words, 5);
    if (url.isEmpty()) {
        emit nothingFound(Source::Wikipedia);
        return;
    }
    QPointer<SelectionLookup> self(this);
    m_transport.get(url, [self, generation, language](const TransportReply &reply) {
        if (!self || generation != self->m_wikipediaGeneration)
            return;
        if (reply.status == 404) {
            emit self->nothingFound(Source::Wikipedia);
            return;
        }
        if (reply.status != 200) {
            emit self->failed(Source::Wikipedia, self->problemWith(reply.status, reply.error));
            return;
        }
        const QList<WikipediaPage> pages = parseWikipediaSearch(reply.body);
        if (pages.isEmpty()) {
            emit self->nothingFound(Source::Wikipedia);
            return;
        }
        self->fetchSummary(pages.first().key, language, pages.mid(1), generation);
    });
}

void SelectionLookup::showWikipediaPage(const QString &key, const QString &language)
{
    fetchSummary(key, language, {}, ++m_wikipediaGeneration);
}

void SelectionLookup::fetchSummary(const QString &key, const QString &language, const QList<WikipediaPage> &others,
                                   int generation)
{
    const QUrl url = wikipediaSummaryUrl(language, key);
    if (url.isEmpty()) {
        emit nothingFound(Source::Wikipedia);
        return;
    }
    QPointer<SelectionLookup> self(this);
    m_transport.get(url, [self, generation, others](const TransportReply &reply) {
        if (!self || generation != self->m_wikipediaGeneration)
            return;
        if (reply.status == 404) {
            emit self->nothingFound(Source::Wikipedia);
            return;
        }
        if (reply.status != 200) {
            emit self->failed(Source::Wikipedia, self->problemWith(reply.status, reply.error));
            return;
        }
        const std::optional<WikipediaSummary> summary = parseWikipediaSummary(reply.body);
        if (!summary) {
            emit self->nothingFound(Source::Wikipedia);
            return;
        }
        emit self->wikipediaFound(*summary, others);
    });
}

void SelectionLookup::lookUpOnWiktionary(const QString &words, const QString &languageFirst)
{
    const int generation = ++m_wiktionaryGeneration;
    const QUrl url = wiktionaryDefinitionUrl(words);
    if (url.isEmpty()) {
        emit nothingFound(Source::Wiktionary);
        return;
    }
    QPointer<SelectionLookup> self(this);
    m_transport.get(url, [self, generation, languageFirst](const TransportReply &reply) {
        if (!self || generation != self->m_wiktionaryGeneration)
            return;
        if (reply.status == 404) {
            emit self->nothingFound(Source::Wiktionary);
            return;
        }
        if (reply.status != 200) {
            emit self->failed(Source::Wiktionary, self->problemWith(reply.status, reply.error));
            return;
        }
        const QList<WiktionaryEntry> entries = parseWiktionary(reply.body, languageFirst);
        if (entries.isEmpty())
            emit self->nothingFound(Source::Wiktionary);
        else
            emit self->wiktionaryFound(entries);
    });
}

} // namespace papertrail::metadata
