#include "core/text/References.h"

#include "core/model/Identifiers.h"

#include <QRegularExpression>

namespace papertrail::core {

namespace {

// A figure's or section's number, such as "3", "3b", "2.1", "A" or "A.2". Letters only in their own case, so that
// "Figure a" isn't read as one.
const QString kNumber = QStringLiteral("(?:[A-Z](?:\\.\\d+)*\\b|\\d+(?:\\.\\d+)*[a-z]?)");
const QString kSeparator = QStringLiteral("\\s*(?:,|;|and|&|or|to|–|—|-)\\s*");

struct Word {
    TextReference::Kind kind;
    QString canonical;
};

// What a word like "Figs." or "Lemma" in a sentence refers to.
std::optional<Word> wordFor(const QString &matched)
{
    using Kind = TextReference::Kind;
    const QString word = matched.toLower();
    if (word.startsWith(QLatin1StringView("fig")))
        return Word{Kind::Figure, QStringLiteral("Figure")};
    if (word.startsWith(QLatin1StringView("tab")))
        return Word{Kind::Table, QStringLiteral("Table")};
    if (word.startsWith(QLatin1StringView("eq")))
        return Word{Kind::Equation, QStringLiteral("Equation")};
    if (word.startsWith(QLatin1StringView("sec")) || word.startsWith(u'§'))
        return Word{Kind::Section, QStringLiteral("Section")};
    if (word.startsWith(QLatin1StringView("app")))
        return Word{Kind::Appendix, QStringLiteral("Appendix")};
    if (word.startsWith(QLatin1StringView("alg")))
        return Word{Kind::Algorithm, QStringLiteral("Algorithm")};
    if (word.startsWith(QLatin1StringView("th")))
        return Word{Kind::Theorem, QStringLiteral("Theorem")};
    if (word.startsWith(QLatin1StringView("lem")))
        return Word{Kind::Theorem, QStringLiteral("Lemma")};
    if (word.startsWith(QLatin1StringView("prop")))
        return Word{Kind::Theorem, QStringLiteral("Proposition")};
    if (word.startsWith(QLatin1StringView("def")))
        return Word{Kind::Theorem, QStringLiteral("Definition")};
    if (word.startsWith(QLatin1StringView("cor")))
        return Word{Kind::Theorem, QStringLiteral("Corollary")};
    return std::nullopt;
}

// The ways a caption or heading can name each kind, as in "Fig. 3" or "Thm. 2".
QString captionWords(const TextReference &reference)
{
    using Kind = TextReference::Kind;
    switch (reference.kind) {
    case Kind::Figure:
        return QStringLiteral("figure|fig\\.?");
    case Kind::Table:
        return QStringLiteral("table|tab\\.");
    case Kind::Algorithm:
        return QStringLiteral("algorithm|alg\\.");
    case Kind::Appendix:
        return QStringLiteral("appendix");
    case Kind::Theorem:
        if (reference.word == QLatin1StringView("Lemma"))
            return QStringLiteral("lemma");
        if (reference.word == QLatin1StringView("Proposition"))
            return QStringLiteral("proposition|prop\\.");
        if (reference.word == QLatin1StringView("Definition"))
            return QStringLiteral("definition|def\\.");
        if (reference.word == QLatin1StringView("Corollary"))
            return QStringLiteral("corollary|cor\\.");
        return QStringLiteral("theorem|thm\\.");
    default:
        return {};
    }
}

// The number at `offset` among those in `list`, which starts at `listStart` in the text: the one there, the one just
// before it if it's between them, or the first if it's before them all.
QString numberAt(const QString &list, qsizetype listStart, int offset, const QRegularExpression &token)
{
    QString chosen;
    for (auto it = token.globalMatch(list); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const qsizetype start = listStart + match.capturedStart(1);
        if (chosen.isEmpty() || start <= offset)
            chosen = match.captured(1);
        if (start > offset)
            break;
    }
    return chosen;
}

bool spans(const QRegularExpressionMatch &match, int offset)
{
    return offset >= match.capturedStart(0) && offset < match.capturedEnd(0);
}

std::optional<TextReference> labelledAt(const QString &text, int offset)
{
    static const QRegularExpression mention(
        QStringLiteral("(?<![\\p{L}])((?i:figures?|figs?\\.?|tables?|tabs?\\.|equations?|eqns?\\.?|eqs?\\.?|sections?|secs?\\.|§§?"
                       "|appendix|appendices|algorithms?|algs?\\.|theorems?|thms?\\.|lemmas?|lemmata|propositions?|props?\\."
                       "|definitions?|defs?\\.|corollary|corollaries))\\s*~?(\\(?%1\\)?(?:%2\\(?%1\\)?)*)")
            .arg(kNumber, kSeparator));
    static const QRegularExpression token(QStringLiteral("\\(?(%1)\\)?").arg(kNumber));
    for (auto it = mention.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (!spans(match, offset))
            continue;
        const std::optional<Word> word = wordFor(match.captured(1));
        if (!word)
            continue;
        TextReference reference;
        reference.kind = word->kind;
        reference.word = word->canonical;
        reference.number = numberAt(match.captured(2), match.capturedStart(2), offset, token);
        if (!reference.number.isEmpty())
            return reference;
    }
    return std::nullopt;
}

std::optional<TextReference> citationAt(const QString &text, int offset)
{
    static const QRegularExpression bracket(
        QStringLiteral("\\[(\\s*\\d{1,4}(?:\\s*[–—-]\\s*\\d{1,4})?(?:\\s*[,;]\\s*\\d{1,4}(?:\\s*[–—-]\\s*\\d{1,4})?)*\\s*)\\]"));
    static const QRegularExpression token(QStringLiteral("(\\d+)"));
    for (auto it = bracket.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (!spans(match, offset))
            continue;
        TextReference reference;
        reference.kind = TextReference::Kind::Citation;
        reference.number = numberAt(match.captured(1), match.capturedStart(1), offset, token);
        return reference;
    }
    return std::nullopt;
}

std::optional<TextReference> authorYearAt(const QString &text, int offset)
{
    // "Smith (2020)", "Smith et al., 2020a", "Garner and Holt (2019)", or one of "(Smith, 2020; Lee 2019)".
    static const QRegularExpression cited(
        QStringLiteral("(?<![\\p{L}])(\\p{Lu}[\\p{L}'’\\-]+)(?:\\s+(?:et\\s+al\\.?|and|&)(?:\\s+\\p{Lu}[\\p{L}'’\\-]+)?)?"
                       "(?:,|\\s)\\s*\\(?((?:1[6-9]|20)\\d\\d[a-z]?)\\b\\)?"));
    // Capitalised words that come before a year without being anyone's name.
    static const QStringList notNames = {
        QStringLiteral("in"), QStringLiteral("since"), QStringLiteral("from"), QStringLiteral("by"), QStringLiteral("until"),
        QStringLiteral("during"), QStringLiteral("before"), QStringLiteral("after"), QStringLiteral("the"),
        QStringLiteral("figure"), QStringLiteral("table"), QStringLiteral("copyright"), QStringLiteral("vol"),
        QStringLiteral("volume"), QStringLiteral("no"), QStringLiteral("issue"), QStringLiteral("january"),
        QStringLiteral("february"), QStringLiteral("march"), QStringLiteral("april"), QStringLiteral("may"),
        QStringLiteral("june"), QStringLiteral("july"), QStringLiteral("august"), QStringLiteral("september"),
        QStringLiteral("october"), QStringLiteral("november"), QStringLiteral("december"), QStringLiteral("spring"),
        QStringLiteral("summer"), QStringLiteral("autumn"), QStringLiteral("fall"), QStringLiteral("winter"),
        QStringLiteral("received"), QStringLiteral("accepted"), QStringLiteral("published"), QStringLiteral("revised")};
    for (auto it = cited.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (!spans(match, offset) || notNames.contains(match.captured(1).toLower()))
            continue;
        TextReference reference;
        reference.kind = TextReference::Kind::AuthorYear;
        reference.surname = match.captured(1);
        reference.year = match.captured(2);
        return reference;
    }
    return std::nullopt;
}

std::optional<TextReference> bareEquationAt(const QString &text, int offset)
{
    static const QRegularExpression bare(QStringLiteral("\\((\\d{1,3}(?:\\.\\d+)?[a-z]?)\\)"));
    for (auto it = bare.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (!spans(match, offset))
            continue;
        TextReference reference;
        reference.kind = TextReference::Kind::Equation;
        reference.word = QStringLiteral("Equation");
        reference.number = match.captured(1);
        return reference;
    }
    return std::nullopt;
}

} // namespace

QString TextReference::label() const
{
    switch (kind) {
    case Kind::Citation:
        return QStringLiteral("reference %1").arg(number);
    case Kind::AuthorYear:
        return QStringLiteral("%1 %2").arg(surname, year);
    default:
        return QStringLiteral("%1 %2").arg(word, number);
    }
}

std::optional<TextReference> referenceAt(const QString &text, int offset)
{
    if (offset < 0 || offset >= text.size())
        return std::nullopt;
    if (auto found = labelledAt(text, offset))
        return found;
    if (auto found = citationAt(text, offset))
        return found;
    if (auto found = authorYearAt(text, offset))
        return found;
    return bareEquationAt(text, offset);
}

bool isTargetLine(const TextReference &reference, const QString &line)
{
    using Kind = TextReference::Kind;
    const QString number = QRegularExpression::escape(reference.number);
    QString pattern;
    if (reference.kind == Kind::Section) {
        // A heading: its number, then a capital, and short. "Section 2" itself, if that's how it's headed.
        if (line.trimmed().size() > 90)
            return false;
        pattern = QStringLiteral("^\\s*(?:(?i:section|sec\\.|§)\\s*)?%1\\.?\\s+\\p{Lu}").arg(number);
    } else if (reference.kind == Kind::Appendix) {
        pattern = QStringLiteral("^\\s*(?i:appendix)\\s+%1(?![\\p{L}\\d])").arg(number);
    } else {
        const QString words = captionWords(reference);
        if (words.isEmpty())
            return false;
        // Then punctuation, a capital, or nothing more: "Figure 3: ...", "Figure 3 | ...", "Table 1 Results ...". In a
        // sentence it's followed by a small letter, as in "Figure 3 shows".
        pattern = QStringLiteral("^\\s*(?i:%1)\\s*%2(?=\\s*[.:|—–\\-)(]|\\s+[\\p{Lu}\\[]|\\s*$)").arg(words, number);
    }
    const QRegularExpression target(pattern);
    return target.match(line).hasMatch();
}

bool endsWithEquationNumber(const QString &line, const QString &number)
{
    const QRegularExpression ending(QStringLiteral("\\(\\s*%1\\s*\\)\\s*$").arg(QRegularExpression::escape(number)));
    return ending.match(line).hasMatch();
}

bool isReferencesHeading(const QString &line)
{
    static const QRegularExpression heading(
        QStringLiteral("^\\s*(?:\\d+\\.?\\s*|[IVX]+\\.\\s*)?(?i:references|bibliography|works cited|literature cited|cited literature"
                       "|reference list|references and notes|literature)\\s*$"));
    return heading.match(line).hasMatch();
}

bool startsNumberedEntry(const QString &line, const QString &number)
{
    const QString n = number.isEmpty() ? QStringLiteral("\\d{1,4}") : QRegularExpression::escape(number);
    const QRegularExpression entry(QStringLiteral("^\\s*(?:\\[%1\\]|%1\\.\\s|%1\\s+(?=\\p{Lu}))").arg(n));
    return entry.match(line).hasMatch();
}

bool startsAuthorEntry(const QString &line, const QString &surname)
{
    static const QRegularExpression leadingNumber(QStringLiteral("^\\s*(?:\\[\\d{1,4}\\]|\\d{1,4}\\.)\\s*"));
    QString rest = line;
    rest.remove(leadingNumber);
    const QString folded = foldedText(rest);
    const QString name = foldedText(surname);
    if (name.isEmpty() || !folded.startsWith(name))
        return false;
    return folded.size() == name.size() || !folded.at(name.size()).isLetter();
}

bool entryHasYear(const QString &entry, const QString &year)
{
    const QRegularExpression wanted(QStringLiteral("(?<!\\d)%1(?![\\d\\p{L}])").arg(QRegularExpression::escape(year)));
    return wanted.match(entry).hasMatch();
}

} // namespace papertrail::core
