#include "core/query/SearchParser.h"

#include "core/model/Identifiers.h"

#include <QRegularExpression>

#include <algorithm>

namespace papertrail::core {

namespace {

struct Token {
    QString field; // Lower case, or empty for plain text.
    QString value;
    bool quoted = false;
};

bool isFieldName(const QString &name)
{
    static const QStringList fields = {
        QStringLiteral("title"), QStringLiteral("author"), QStringLiteral("abstract"),
        QStringLiteral("journal"), QStringLiteral("conference"), QStringLiteral("publisher"),
        QStringLiteral("institution"), QStringLiteral("tag"), QStringLiteral("doi"),
        QStringLiteral("isbn"), QStringLiteral("issn"), QStringLiteral("arxiv"),
        QStringLiteral("type"), QStringLiteral("year"),
    };
    return fields.contains(name);
}

QList<Token> tokenise(const QString &text)
{
    QList<Token> tokens;
    qsizetype i = 0;
    const qsizetype size = text.size();
    while (i < size) {
        while (i < size && text.at(i).isSpace())
            ++i;
        if (i >= size)
            break;

        Token token;
        if (text.at(i) != u'"') {
            // Read a word, stopping early if it turns out to be a known field name.
            const qsizetype start = i;
            while (i < size && !text.at(i).isSpace() && text.at(i) != u':')
                ++i;
            if (i < size && text.at(i) == u':') {
                const QString name = text.mid(start, i - start).toLower();
                if (isFieldName(name)) {
                    token.field = name;
                    ++i;
                } else {
                    i = start;
                }
            } else {
                i = start;
            }
        }

        if (i < size && text.at(i) == u'"') {
            const qsizetype end = text.indexOf(u'"', i + 1);
            const qsizetype stop = end < 0 ? size : end;
            token.value = text.mid(i + 1, stop - i - 1);
            token.quoted = true;
            i = end < 0 ? size : end + 1;
        } else {
            const qsizetype start = i;
            while (i < size && !text.at(i).isSpace())
                ++i;
            token.value = text.mid(start, i - start);
        }

        if (!token.value.trimmed().isEmpty())
            tokens << token;
    }
    return tokens;
}

bool hasSearchableText(const QString &text)
{
    return std::any_of(text.begin(), text.end(), [](QChar c) { return c.isLetterOrNumber(); });
}

// FTS5 treats everything inside double quotes as text, so this is safe for any input.
QString ftsTerm(const QString &text, bool prefix)
{
    QString escaped = text;
    escaped.replace(u'"', QStringLiteral("\"\""));
    return u'"' + escaped + u'"' + (prefix ? QStringLiteral("*") : QString());
}

QString ftsColumn(const QString &field)
{
    if (field == QStringLiteral("author"))
        return QStringLiteral("authors");
    if (field == QStringLiteral("journal") || field == QStringLiteral("conference"))
        return QStringLiteral("container");
    if (field == QStringLiteral("publisher") || field == QStringLiteral("institution"))
        return QStringLiteral("publisher");
    return field; // title, abstract
}

void addFtsTerms(QStringList &terms, const Token &token)
{
    const QString column = token.field.isEmpty() ? QString() : ftsColumn(token.field) + QStringLiteral(" : ");

    // Names can be written in either order, so an author's words are matched separately.
    const bool splitWords = token.field == QStringLiteral("author") || !token.quoted;
    if (splitWords) {
        const QStringList words = token.value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString &word : words) {
            if (hasSearchableText(word))
                terms << column + ftsTerm(word, true);
        }
    } else if (hasSearchableText(token.value)) {
        terms << column + ftsTerm(token.value, false);
    }
}

// Returns false if the value isn't a year expression, leaving the bounds alone.
bool parseYear(const QString &value, ParsedSearch &result)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(?:(>=|<=|>|<)?(\\d{1,4})|(\\d{1,4})\\.\\.(\\d{1,4}))$"));
    const QRegularExpressionMatch match = pattern.match(value.trimmed());
    if (!match.hasMatch())
        return false;

    if (!match.captured(3).isEmpty()) {
        result.yearMin = match.captured(3).toInt();
        result.yearMax = match.captured(4).toInt();
        return true;
    }

    const int year = match.captured(2).toInt();
    const QString op = match.captured(1);
    if (op == QStringLiteral(">"))
        result.yearMin = year + 1;
    else if (op == QStringLiteral(">="))
        result.yearMin = year;
    else if (op == QStringLiteral("<"))
        result.yearMax = year - 1;
    else if (op == QStringLiteral("<="))
        result.yearMax = year;
    else
        result.yearMin = result.yearMax = year;
    return true;
}

// Matches the stored name or any word in it, so type:thesis and type:journal both work.
QList<PublicationType> matchTypes(const QString &value)
{
    QString wanted = value.trimmed().toLower();
    wanted.replace(u' ', u'_');
    QList<PublicationType> matches;
    for (const PublicationType type : allPublicationTypes()) {
        const QString name = toDbString(type);
        if (name == wanted || name.split(u'_').contains(wanted))
            matches << type;
    }
    return matches;
}

} // namespace

ParsedSearch parseSearch(const QString &text)
{
    ParsedSearch result;
    QStringList ftsTerms;
    QStringList contentsTerms;
    QStringList fieldTerms;
    QString phrase;
    QString longestWord;

    // Words for the metadata, and also noted for searching inside documents if they're plain ones.
    const auto addWords = [&](const Token &token) {
        addFtsTerms(ftsTerms, token);
        if (!token.field.isEmpty()) {
            addFtsTerms(fieldTerms, token);
            return;
        }
        addFtsTerms(contentsTerms, token);
        if (token.quoted && phrase.isEmpty() && hasSearchableText(token.value))
            phrase = token.value.trimmed();
        const QStringList words = token.value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString &word : words) {
            if (hasSearchableText(word) && word.size() > longestWord.size())
                longestWord = word;
        }
    };

    for (const Token &token : tokenise(text)) {
        if (token.field == QStringLiteral("tag")) {
            result.tagPrefixes << token.value.trimmed();
        } else if (token.field == QStringLiteral("doi")) {
            result.doiPrefixes << normaliseDoi(token.value);
        } else if (token.field == QStringLiteral("isbn")) {
            result.isbns << normaliseIsbn(token.value);
        } else if (token.field == QStringLiteral("issn")) {
            result.issns << normaliseIssn(token.value);
        } else if (token.field == QStringLiteral("arxiv")) {
            result.arxivPrefixes << normaliseArxivId(token.value);
        } else if (token.field == QStringLiteral("type")) {
            const QList<PublicationType> types = matchTypes(token.value);
            if (types.isEmpty())
                addWords(Token{QString(), token.value, token.quoted});
            else
                result.types << types;
        } else if (token.field == QStringLiteral("year")) {
            if (!parseYear(token.value, result))
                addWords(Token{QString(), token.value, token.quoted});
        } else {
            addWords(token);
        }
    }

    result.ftsMatch = ftsTerms.join(u' ');
    result.contentsMatch = contentsTerms.join(u' ');
    result.fieldMatch = fieldTerms.join(u' ');
    result.contentsText = phrase.isEmpty() ? longestWord : phrase;
    return result;
}

} // namespace papertrail::core
