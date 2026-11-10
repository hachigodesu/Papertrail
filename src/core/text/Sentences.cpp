#include "core/text/Sentences.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTextBoundaryFinder>

#include <algorithm>

namespace papertrail::core {

namespace {

// Where each character of the joined text came from: a line and a character in it, or -1 for a space put between
// lines.
struct Origin {
    int line = -1;
    int index = -1;
};

// Words that end with a full stop without ending a sentence, in lower case and without the stop.
bool endsWithAbbreviation(const QString &sentence)
{
    static const QRegularExpression lastWord(QStringLiteral("(\\S+)\\.\\s*$"));
    const QRegularExpressionMatch match = lastWord.match(sentence);
    if (!match.hasMatch())
        return false;
    QString word = match.captured(1);
    // Brackets and quotes before it, as in "(Fig." or "[cf.".
    while (!word.isEmpty() && !word.at(0).isLetterOrNumber())
        word.remove(0, 1);
    static const QStringList abbreviations = {
        QStringLiteral("al"),   QStringLiteral("fig"),  QStringLiteral("figs"), QStringLiteral("eq"),   QStringLiteral("eqs"),
        QStringLiteral("e.g"),  QStringLiteral("i.e"),  QStringLiteral("cf"),   QStringLiteral("vs"),   QStringLiteral("etc"),
        QStringLiteral("no"),   QStringLiteral("nos"),  QStringLiteral("vol"),  QStringLiteral("vols"), QStringLiteral("pp"),
        QStringLiteral("p"),    QStringLiteral("ch"),   QStringLiteral("sec"),  QStringLiteral("ref"),  QStringLiteral("refs"),
        QStringLiteral("dr"),   QStringLiteral("mr"),   QStringLiteral("mrs"),  QStringLiteral("ms"),   QStringLiteral("prof"),
        QStringLiteral("st"),   QStringLiteral("approx"), QStringLiteral("ed"), QStringLiteral("eds"),  QStringLiteral("tab"),
        QStringLiteral("viz"),  QStringLiteral("resp"), QStringLiteral("cit"),  QStringLiteral("op"),   QStringLiteral("ibid"),
    };
    if (abbreviations.contains(word.toLower()))
        return true;
    // An initial, such as the "J." of "J. Smith".
    return word.size() == 1 && word.at(0).isUpper();
}

// A lone number or reference marker, such as "[12]", isn't worth reading out.
bool hasWords(const QString &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.isLetter(); });
}

// The boxes of these characters, a box for each line they're on.
QList<QRectF> rectsOf(const QList<SentenceLine> &lines, const QList<Origin> &origins, qsizetype from, qsizetype to)
{
    QList<QRectF> rects;
    int line = -1;
    for (qsizetype i = from; i < to; ++i) {
        const Origin &origin = origins.at(i);
        if (origin.line < 0 || origin.index < 0 || origin.index >= lines.at(origin.line).chars.size())
            continue;
        const QRectF box = lines.at(origin.line).chars.at(origin.index);
        if (box.isEmpty() && box.width() <= 0)
            continue;
        if (origin.line != line) {
            rects << box;
            line = origin.line;
        } else {
            rects.last() = rects.last().united(box);
        }
    }
    return rects;
}

// One paragraph's text, joined from its lines, split into sentences.
void splitParagraph(const QList<SentenceLine> &lines, const QList<int> &which, QList<Sentence> &out)
{
    QString text;
    QList<Origin> origins;
    for (qsizetype n = 0; n < which.size(); ++n) {
        const int line = which.at(n);
        QString words = lines.at(line).text;
        // Trailing spaces aren't part of what's read, and leave the boxes as they are.
        qsizetype length = words.size();
        while (length > 0 && words.at(length - 1).isSpace())
            --length;
        // A word broken across lines is joined again: "exam-" then "ple".
        const bool broken = n + 1 < which.size() && length > 1 && words.at(length - 1) == u'-' && words.at(length - 2).isLetter()
                            && !lines.at(which.at(n + 1)).text.isEmpty() && lines.at(which.at(n + 1)).text.at(0).isLower();
        const qsizetype kept = broken ? length - 1 : length;
        for (qsizetype i = 0; i < kept; ++i) {
            text += words.at(i);
            origins << Origin{line, int(i)};
        }
        if (!broken && n + 1 < which.size()) {
            text += u' ';
            origins << Origin{};
        }
    }

    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, text);
    qsizetype start = 0;
    qsizetype pending = -1; // The start of a sentence held back, since a stop after an abbreviation didn't end it.
    while (start < text.size()) {
        qsizetype end = finder.toNextBoundary();
        if (end < 0)
            end = text.size();
        const qsizetype from = pending >= 0 ? pending : start;
        const QString piece = text.mid(from, end - from);
        if (end < text.size() && endsWithAbbreviation(piece)) {
            pending = from;
            start = end;
            continue;
        }
        pending = -1;
        const QString said = piece.simplified();
        if (hasWords(said)) {
            Sentence sentence;
            sentence.text = said;
            sentence.rects = rectsOf(lines, origins, from, end);
            for (qsizetype i = from; i < end; ++i) {
                if (origins.at(i).line >= 0) {
                    sentence.firstLine = origins.at(i).line;
                    break;
                }
            }
            out << sentence;
        }
        start = end;
    }
}

} // namespace

QList<Sentence> sentencesOf(const QList<SentenceLine> &lines)
{
    QList<Sentence> sentences;
    QList<int> paragraph;
    for (int i = 0; i < lines.size(); ++i) {
        const SentenceLine &line = lines.at(i);
        if (line.margin || line.text.trimmed().isEmpty())
            continue;
        if (line.paragraph && !paragraph.isEmpty()) {
            splitParagraph(lines, paragraph, sentences);
            paragraph.clear();
        }
        paragraph << i;
    }
    if (!paragraph.isEmpty())
        splitParagraph(lines, paragraph, sentences);
    return sentences;
}

} // namespace papertrail::core
