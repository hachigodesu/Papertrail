#include "citation/Formatting.h"

#include <QRegularExpression>
#include <QStringList>

namespace papertrail::citation {

RichText &RichText::add(const QString &text, bool italicText)
{
    if (text.isEmpty())
        return *this;
    if (!m_runs.isEmpty() && m_runs.last().italic == italicText)
        m_runs.last().text += text;
    else
        m_runs << Run{text, italicText};
    return *this;
}

RichText &RichText::append(const RichText &other)
{
    for (const Run &run : other.m_runs)
        add(run.text, run.italic);
    return *this;
}

QChar RichText::lastChar() const
{
    for (qsizetype i = m_runs.size() - 1; i >= 0; --i) {
        const QString trimmed = m_runs.at(i).text;
        for (qsizetype j = trimmed.size() - 1; j >= 0; --j) {
            if (!trimmed.at(j).isSpace())
                return trimmed.at(j);
        }
    }
    return {};
}

RichText &RichText::endSentence()
{
    if (isEmpty())
        return *this;
    // Trailing spaces go, so the stop sits against the text.
    while (!m_runs.isEmpty()) {
        QString &text = m_runs.last().text;
        while (!text.isEmpty() && text.back().isSpace())
            text.chop(1);
        if (!text.isEmpty())
            break;
        m_runs.removeLast();
    }
    const QChar last = lastChar();
    if (last != u'.' && last != u'?' && last != u'!')
        add(QStringLiteral("."));
    return *this;
}

bool RichText::isEmpty() const
{
    for (const Run &run : m_runs) {
        if (!run.text.trimmed().isEmpty())
            return false;
    }
    return true;
}

QString RichText::plain() const
{
    QString text;
    for (const Run &run : m_runs)
        text += run.text;
    return text.simplified();
}

QString RichText::html() const
{
    QString text;
    for (const Run &run : m_runs)
        text += run.italic ? QStringLiteral("<i>") + run.text.toHtmlEscaped() + QStringLiteral("</i>") : run.text.toHtmlEscaped();
    return text.simplified();
}

QString initials(const QString &given, bool spaced)
{
    QStringList parts;
    // Words are split on spaces and full stops, and hyphenated names keep their hyphen between initials.
    static const QRegularExpression words(QStringLiteral("[\\s.]+"));
    for (const QString &word : given.split(words, Qt::SkipEmptyParts)) {
        QStringList pieces;
        for (const QString &piece : word.split(u'-', Qt::SkipEmptyParts)) {
            const QChar first = piece.at(0);
            if (!first.isLetter())
                continue;
            pieces << QString(first.toUpper()) + u'.';
        }
        if (!pieces.isEmpty())
            parts << pieces.join(u'-');
    }
    return parts.join(spaced ? QStringLiteral(" ") : QString());
}

bool isOrganisation(const core::PersonName &person)
{
    return person.given.trimmed().isEmpty();
}

bool isCjkName(const core::PersonName &person)
{
    for (const QChar c : person.family + person.given) {
        const QChar::Script script = c.script();
        if (script == QChar::Script_Han || script == QChar::Script_Hiragana || script == QChar::Script_Katakana
            || script == QChar::Script_Hangul)
            return true;
    }
    return false;
}

QString familyName(const core::PersonName &person)
{
    if (isCjkName(person))
        return person.family.trimmed() + person.given.trimmed();
    return person.family.trimmed();
}

std::optional<PageRange> pageRange(const QString &pages)
{
    const QString text = pages.simplified();
    if (text.isEmpty())
        return std::nullopt;
    static const QRegularExpression range(QStringLiteral("^([A-Za-z]{0,3}\\d+[A-Za-z]{0,2})\\s*(?:-+|–|—|‒|‐|‑)\\s*([A-Za-z]{0,3}\\d+[A-Za-z]{0,2})$"));
    if (const QRegularExpressionMatch match = range.match(text); match.hasMatch()) {
        if (match.captured(1) == match.captured(2))
            return PageRange{match.captured(1), {}};
        return PageRange{match.captured(1), match.captured(2)};
    }
    static const QRegularExpression single(QStringLiteral("^[A-Za-z]{0,3}\\d+[A-Za-z]{0,2}$"));
    if (single.match(text).hasMatch())
        return PageRange{text, {}};
    return std::nullopt;
}

QString joinedPages(const QString &pages, const QString &dash)
{
    const std::optional<PageRange> range = pageRange(pages);
    if (!range)
        return pages.simplified();
    return range->isRange() ? range->first + dash + range->last : range->first;
}

QString chicagoPages(const QString &pages)
{
    const std::optional<PageRange> range = pageRange(pages);
    if (!range)
        return pages.simplified();
    if (!range->isRange())
        return range->first;
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    const QString &first = range->first;
    const QString &last = range->last;
    const QString dash = QStringLiteral("–");
    if (!digits.match(first).hasMatch() || !digits.match(last).hasMatch() || last.size() != first.size()
        || last.toLongLong() <= first.toLongLong())
        return first + dash + last;

    const qlonglong start = first.toLongLong();
    // Under 100, or a multiple of 100: all of it.
    if (start < 100 || start % 100 == 0)
        return first + dash + last;
    // The first differing digit onwards, keeping at least two digits after 10 in the hundreds, as in 321–28.
    qsizetype same = 0;
    while (same < first.size() && first.at(same) == last.at(same))
        ++same;
    QString shortened = last.mid(same);
    if (start % 100 >= 10 && shortened.size() < 2)
        shortened = last.right(2);
    return first + dash + shortened;
}

QString englishMonth(int month)
{
    static const QStringList months = {QStringLiteral("January"), QStringLiteral("February"), QStringLiteral("March"),
                                       QStringLiteral("April"),   QStringLiteral("May"),      QStringLiteral("June"),
                                       QStringLiteral("July"),    QStringLiteral("August"),   QStringLiteral("September"),
                                       QStringLiteral("October"), QStringLiteral("November"), QStringLiteral("December")};
    return month >= 1 && month <= 12 ? months.at(month - 1) : QString();
}

QString englishShortMonth(int month)
{
    static const QStringList months = {QStringLiteral("Jan."), QStringLiteral("Feb."),  QStringLiteral("Mar."),
                                       QStringLiteral("Apr."), QStringLiteral("May"),   QStringLiteral("June"),
                                       QStringLiteral("July"), QStringLiteral("Aug."),  QStringLiteral("Sept."),
                                       QStringLiteral("Oct."), QStringLiteral("Nov."),  QStringLiteral("Dec.")};
    return month >= 1 && month <= 12 ? months.at(month - 1) : QString();
}

QString accessedDate(int day, int month, int year)
{
    return QStringLiteral("%1 %2 %3").arg(day).arg(englishMonth(month)).arg(year);
}

QString ordinal(const QString &number)
{
    bool ok = false;
    const int n = number.trimmed().toInt(&ok);
    if (!ok || n <= 0)
        return number.trimmed();
    const int lastTwo = n % 100;
    const int last = n % 10;
    QString suffix = QStringLiteral("th");
    if (lastTwo < 11 || lastTwo > 13) {
        if (last == 1)
            suffix = QStringLiteral("st");
        else if (last == 2)
            suffix = QStringLiteral("nd");
        else if (last == 3)
            suffix = QStringLiteral("rd");
    }
    return QString::number(n) + suffix;
}

QString editionText(const QString &edition, const QString &abbreviation)
{
    QString text = edition.simplified();
    if (text.isEmpty())
        return {};
    static const QRegularExpression firstEdition(QStringLiteral("^(1|1st|first)(\\s*(ed\\.?|edition))?$"),
                                                 QRegularExpression::CaseInsensitiveOption);
    if (firstEdition.match(text).hasMatch())
        return {};
    static const QRegularExpression numbered(QStringLiteral("^(\\d+)(st|nd|rd|th)?(\\s*(ed\\.?|edition))?$"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch match = numbered.match(text); match.hasMatch())
        return ordinal(match.captured(1)) + u' ' + abbreviation;
    static const QRegularExpression mentionsEdition(QStringLiteral("\\b(ed\\.?|edition)$"), QRegularExpression::CaseInsensitiveOption);
    if (mentionsEdition.match(text).hasMatch())
        return text;
    return text + u' ' + abbreviation;
}

DegreeLevel degreeLevel(const QString &degree, core::PublicationType type)
{
    const QString text = degree.toLower().remove(u'.').simplified();
    static const QRegularExpression doctoral(QStringLiteral("\\b(phd|dphil|doctor|doctoral|doctorate|edd|dsc|dba|engd)\\b"));
    static const QRegularExpression masters(QStringLiteral("\\b(master|masters|ma|ms|msc|mphil|mres|meng|mba|med|llm|mst)\\b"));
    if (doctoral.match(text).hasMatch())
        return DegreeLevel::Doctoral;
    if (masters.match(text).hasMatch())
        return DegreeLevel::Masters;
    Q_UNUSED(type)
    return DegreeLevel::Unknown;
}

QString sortKey(const QString &text)
{
    QString key = text.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(key.size());
    for (const QChar c : key) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        if (c.isLetterOrNumber() || c.isSpace())
            folded += c.toCaseFolded();
    }
    return folded.simplified();
}

} // namespace papertrail::citation
