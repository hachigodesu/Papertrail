#include "core/model/Identifiers.h"

#include <QRegularExpression>

namespace papertrail::core {

namespace {

QString stripPrefix(QString value, const QRegularExpression &prefix)
{
    value.remove(prefix);
    return value;
}

QString foldForSort(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing)
            folded.append(c);
    }
    return folded.toCaseFolded().simplified();
}

} // namespace

QString normaliseDoi(const QString &value)
{
    static const QRegularExpression prefix(
        QStringLiteral("^(https?://(dx\\.)?doi\\.org/|doi:\\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString cleaned = stripPrefix(value.trimmed(), prefix).trimmed();
    return cleaned.isEmpty() ? QString() : cleaned.toLower();
}

QString normaliseIsbn(const QString &value)
{
    QString digits;
    for (const QChar c : value) {
        if (c.isDigit() || c == u'x' || c == u'X')
            digits.append(c.toUpper());
    }
    return digits.isEmpty() ? QString() : digits;
}

QString normaliseIssn(const QString &value)
{
    const QString digits = normaliseIsbn(value);
    if (digits.size() == 8)
        return digits.left(4) + u'-' + digits.mid(4);
    return digits;
}

QString normaliseArxivId(const QString &value)
{
    static const QRegularExpression prefix(
        QStringLiteral("^(https?://arxiv\\.org/(abs|pdf)/|arxiv:\\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression pdfSuffix(QStringLiteral("\\.pdf$"),
                                              QRegularExpression::CaseInsensitiveOption);
    QString cleaned = stripPrefix(value.trimmed(), prefix).trimmed();
    cleaned.remove(pdfSuffix);
    return cleaned.isEmpty() ? QString() : cleaned.toLower();
}

QString titleSortKey(const QString &title)
{
    static const QRegularExpression article(QStringLiteral("^(the|an|a)\\s+"));
    QString key = foldForSort(title);
    key.remove(article);
    return key;
}

QString nameSortKey(const QString &name)
{
    return foldForSort(name);
}

} // namespace papertrail::core
