#include "engine/WordLayer.h"

#include <QStringList>

namespace papertrail::engine::words {

namespace {

double distanceTo(const QRectF &rect, const QPointF &point)
{
    const double dx = qMax(qMax(rect.left() - point.x(), point.x() - rect.right()), 0.0);
    const double dy = qMax(qMax(rect.top() - point.y(), point.y() - rect.bottom()), 0.0);
    return dx * dx + dy * dy;
}

} // namespace

QString pageText(const QList<TextWord> &words)
{
    QString text;
    for (const TextWord &word : words) {
        text += word.text;
        text += word.endsLine ? u'\n' : u' ';
    }
    return text;
}

QList<QRectF> search(const QList<TextWord> &words, const QString &query)
{
    const QStringList wanted = query.simplified().split(u' ', Qt::SkipEmptyParts);
    if (wanted.isEmpty())
        return {};
    constexpr int kMaxHits = 512;

    // Words are matched in sequence, so that a phrase can run across a line break.
    QList<QRectF> rects;
    for (qsizetype start = 0; start + wanted.size() <= words.size() && rects.size() < kMaxHits; ++start) {
        bool matched = true;
        for (qsizetype i = 0; i < wanted.size() && matched; ++i) {
            const QString &word = words.at(start + i).text;
            const bool last = i == wanted.size() - 1;
            const bool first = i == 0;
            matched = (first && last) ? word.contains(wanted.at(i), Qt::CaseInsensitive)
                : first               ? word.endsWith(wanted.at(i), Qt::CaseInsensitive)
                : last                ? word.startsWith(wanted.at(i), Qt::CaseInsensitive)
                                      : word.compare(wanted.at(i), Qt::CaseInsensitive) == 0;
        }
        if (matched) {
            for (qsizetype i = 0; i < wanted.size(); ++i)
                rects << words.at(start + i).rect;
        }
    }
    return rects;
}

QList<TextLine> textLines(const QList<TextWord> &words)
{
    // With no fonts, a line's height stands in for its size.
    QList<TextLine> lines;
    TextLine line;
    const auto finish = [&] {
        line.size = line.rect.height() * 0.8;
        if (!line.text.trimmed().isEmpty())
            lines << line;
        line = TextLine();
    };
    for (const TextWord &word : words) {
        if (!line.text.isEmpty()) {
            line.text += u' ';
            const QRectF previous = line.chars.last();
            line.chars << QRectF(previous.right(), previous.top(), qMax(0.0, word.rect.left() - previous.right()), previous.height());
        }
        const qsizetype count = qMax<qsizetype>(1, word.text.size());
        const double width = word.rect.width() / double(count);
        for (qsizetype i = 0; i < word.text.size(); ++i) {
            line.text += word.text.at(i);
            line.chars << QRectF(word.rect.left() + width * double(i), word.rect.top(), width, word.rect.height());
        }
        line.rect = line.rect.isNull() ? word.rect : line.rect.united(word.rect);
        if (word.endsLine)
            finish();
    }
    finish();
    return lines;
}

TextSelection selectText(const QList<TextWord> &words, const QPointF &a, const QPointF &b)
{
    if (words.isEmpty())
        return {};
    const auto nearest = [&](const QPointF &point) {
        qsizetype best = 0;
        double bestDistance = distanceTo(words.first().rect, point);
        for (qsizetype i = 1; i < words.size() && bestDistance > 0; ++i) {
            const double distance = distanceTo(words.at(i).rect, point);
            if (distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        return best;
    };
    qsizetype first = nearest(a);
    qsizetype last = nearest(b);
    if (first > last)
        std::swap(first, last);

    TextSelection selection;
    for (qsizetype i = first; i <= last; ++i) {
        const TextWord &word = words.at(i);
        selection.rects << word.rect;
        selection.text += word.text;
        if (i < last)
            selection.text += word.endsLine ? u'\n' : u' ';
    }
    return selection;
}

QString largestText(const QList<TextWord> &words)
{
    QString best;
    double tallest = 0;
    for (const TextLine &line : textLines(words)) {
        if (line.rect.height() > tallest * 1.05) {
            tallest = line.rect.height();
            best = line.text;
        }
    }
    return best.simplified();
}

} // namespace papertrail::engine::words
