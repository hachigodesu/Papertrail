#pragma once

#include "engine/Document.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

// A page's text as words with boxes, which is what a DjVu file's hidden text and recognised text both are. These
// answer a document's text questions from them, as a PDF's own text answers them.
namespace papertrail::engine {

// One word of a page's text, with its box in points from the page's top left.
struct TextWord {
    QRectF rect;
    QString text;
    bool endsLine = false; // The last word of its line.
};

namespace words {

// The page's text, a line at a time.
QString pageText(const QList<TextWord> &words);

// Where these words are, in sequence, so a phrase can run over a line break. Ignores case.
QList<QRectF> search(const QList<TextWord> &words, const QString &query);

// Lines, with a box for each character: each word's box is shared out evenly among its characters, and a line's
// height stands in for its type's size.
QList<TextLine> textLines(const QList<TextWord> &words);

// The words from the one nearest `a` to the one nearest `b`.
TextSelection selectText(const QList<TextWord> &words, const QPointF &a, const QPointF &b);

// The words of the line set largest, which on a first page is often the title.
QString largestText(const QList<TextWord> &words);

} // namespace words
} // namespace papertrail::engine
