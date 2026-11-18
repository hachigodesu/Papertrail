#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace papertrail::metadata {

class Transport;

// Looking up words chosen in a document on Wikipedia and Wiktionary. Only the words go, and only once the user asks,
// with lookups online. Everything that comes back is someone else's text, so it's kept as plain text.

// The words of a selection that are sent: its text made one line, without quotes or the punctuation around it, and
// cut to at most `maxWords` words and `maxChars` characters. Empty if nothing's left.
QString lookupWords(const QString &selection, int maxWords, int maxChars);

// How many words the words for a lookup have.
int wordCount(const QString &words);

// Whether this is a Wikipedia's language code, as in https://en.wikipedia.org. Only two or three small letters.
bool isWikiLanguage(const QString &code);

// The Wikipedia for this locale's language, as "en" for en_GB, or English where the language has no code to use.
QString wikiLanguageFor(const QString &localeName);

// Addresses, all built here from the words or a page's key, each part encoded so nothing in it can change the address.
QUrl wikipediaSearchUrl(const QString &language, const QString &words, int limit);
QUrl wikipediaSummaryUrl(const QString &language, const QString &key);
QUrl wikipediaPageLink(const QString &language, const QString &key);
QUrl wiktionaryDefinitionUrl(const QString &term);
QUrl wiktionaryPageLink(const QString &term);

// Plain text from a bit of HTML in an answer: tags dropped, and the usual character references read.
QString plainFromHtml(const QString &html);

struct WikipediaPage {
    QString key;         // As the page's address has it, such as "Grain_boundary".
    QString title;
    QString description; // A few words saying what it is, if there are any.
};

struct WikipediaSummary {
    QString key;
    QString title;
    QString description;
    QString extract;            // The start of the article, as plain text.
    bool disambiguation = false; // A page listing several things the words could mean.
};

struct WiktionaryEntry {
    QString language;     // The language's name, such as "English".
    QString languageCode; // Such as "en".
    QString partOfSpeech; // Such as "Noun".
    QStringList definitions;
};

// What came back, or nothing if it isn't what these sources answer.
QList<WikipediaPage> parseWikipediaSearch(const QByteArray &body);
std::optional<WikipediaSummary> parseWikipediaSummary(const QByteArray &body);
// Entries in this language come first, then the rest in the order they came.
QList<WiktionaryEntry> parseWiktionary(const QByteArray &body, const QString &languageFirst);

// One lookup at a time on each source: asking again drops what the last one would have said.
class SelectionLookup : public QObject {
    Q_OBJECT

public:
    enum class Source { Wikipedia, Wiktionary };
    Q_ENUM(Source)

    explicit SelectionLookup(Transport &transport, QObject *parent = nullptr);

    // Searches the language's Wikipedia for the words, then fetches the best match's summary.
    void lookUpOnWikipedia(const QString &words, const QString &language);
    // A page found by the search, chosen instead of the best match.
    void showWikipediaPage(const QString &key, const QString &language);
    // Wiktionary's definitions, which only en.wiktionary gives, for words in any language.
    void lookUpOnWiktionary(const QString &words, const QString &languageFirst);
    void cancel();

signals:
    // `others` are the rest of what the search found, for choosing another.
    void wikipediaFound(const papertrail::metadata::WikipediaSummary &summary,
                        const QList<papertrail::metadata::WikipediaPage> &others);
    void wiktionaryFound(const QList<papertrail::metadata::WiktionaryEntry> &entries);
    void nothingFound(papertrail::metadata::SelectionLookup::Source source);
    void failed(papertrail::metadata::SelectionLookup::Source source, const QString &message);

private:
    void fetchSummary(const QString &key, const QString &language, const QList<WikipediaPage> &others, int generation);
    QString problemWith(int status, const QString &error) const;

    Transport &m_transport;
    int m_wikipediaGeneration = 0;
    int m_wiktionaryGeneration = 0;
};

} // namespace papertrail::metadata
