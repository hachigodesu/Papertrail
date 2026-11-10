#pragma once

#include <QList>
#include <QRectF>
#include <QString>

// Splitting a page's text into sentences for reading aloud, keeping where each is on the page so it can be marked.
namespace papertrail::core {

// One line of a page, as the reader has it: its text, a box for each character, and what kind of line it is.
struct SentenceLine {
    QString text;
    QList<QRectF> chars;
    bool paragraph = false; // Starts a paragraph, so a sentence never runs on into it.
    bool margin = false;    // A running head or page number, which isn't read.
};

struct Sentence {
    QString text;         // As it's to be said: lines joined, and words broken across lines joined again.
    QList<QRectF> rects;  // Where it is on the page, a box for each line it's on.
    int firstLine = 0;    // Which of the lines it starts on.
};

// The sentences on a page, in order. Running heads and page numbers are left out, a sentence never runs from one
// paragraph into the next, and a full stop after "et al.", "Fig.", "e.g." or an initial doesn't end one. Anything
// with no letters, such as a lone reference marker, isn't a sentence.
QList<Sentence> sentencesOf(const QList<SentenceLine> &lines);

} // namespace papertrail::core
