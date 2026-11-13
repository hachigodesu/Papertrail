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

// Whether a sentence ends as one does: with a full stop, a question or exclamation mark, or an ellipsis, perhaps
// followed by closing quotes or brackets. A page's last sentence that doesn't carries on over the page.
bool endsSentence(const QString &sentence);
// A sentence carried on over a page, from its words on the one page and the other, with a word broken across them
// joined again.
QString joinedAcrossPages(const QString &before, const QString &after);

// A sentence as a voice should be given it: abbreviations that voices misread said in full ("Fig. 3" as "Figure 3",
// "pp. 113-118" as "pages 113 to 118", "e.g." as "for example"), and reference markers such as "[12]" left out. What's
// marked on the page stays as it is.
QString spokenForm(const QString &sentence);

} // namespace papertrail::core
