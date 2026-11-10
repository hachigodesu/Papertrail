// Splitting a page's lines into sentences to read aloud, and keeping where each is.

#include "core/text/Sentences.h"

#include <QTest>

using namespace papertrail::core;

namespace {

// A line at this height, each character 6 points wide.
SentenceLine line(const QString &text, double y, bool paragraph = false, bool margin = false)
{
    SentenceLine made;
    made.text = text;
    for (qsizetype i = 0; i < text.size(); ++i)
        made.chars << QRectF(50 + 6 * i, y, 6, 10);
    made.paragraph = paragraph;
    made.margin = margin;
    return made;
}

QStringList texts(const QList<Sentence> &sentences)
{
    QStringList out;
    for (const Sentence &sentence : sentences)
        out << sentence.text;
    return out;
}

} // namespace

class SentencesTest : public QObject {
    Q_OBJECT

private slots:
    void sentencesRunAcrossLinesAndStopAtTheirEnds()
    {
        const QList<Sentence> found = sentencesOf({line(QStringLiteral("The beam bends under a load. It"), 100, true),
                                                   line(QStringLiteral("breaks if the load is too great."), 112)});
        QCOMPARE(texts(found), (QStringList{QStringLiteral("The beam bends under a load."),
                                            QStringLiteral("It breaks if the load is too great.")}));
        // The first is on one line, and the second on two.
        QCOMPARE(found.at(0).rects.size(), qsizetype(1));
        QCOMPARE(found.at(1).rects.size(), qsizetype(2));
        QCOMPARE(found.at(1).firstLine, 0);
        QCOMPARE(found.at(1).rects.first().left(), 50.0 + 6 * 29);
        QCOMPARE(found.at(1).rects.last().top(), 112.0);
    }

    void abbreviationsAndInitialsDontEndASentence()
    {
        const QList<Sentence> found = sentencesOf({line(QStringLiteral("As Smith et al. showed in Fig. 3, the made-up beam"), 100),
                                                   line(QStringLiteral("bends, e.g. under J. Example's load. Then it stops."), 112)});
        QCOMPARE(texts(found), (QStringList{QStringLiteral("As Smith et al. showed in Fig. 3, the made-up beam bends, e.g. under J. "
                                                           "Example's load."),
                                            QStringLiteral("Then it stops.")}));
    }

    void wordsBrokenAcrossLinesAreJoined()
    {
        const QList<Sentence> found = sentencesOf({line(QStringLiteral("The made-up exam-"), 100), line(QStringLiteral("ple ends here."), 112)});
        QCOMPARE(texts(found), QStringList{QStringLiteral("The made-up example ends here.")});
        // A hyphen in a word on one line stays, and so does one before a capital.
        QCOMPARE(texts(sentencesOf({line(QStringLiteral("The North-"), 100), line(QStringLiteral("South line."), 112)})),
                 QStringList{QStringLiteral("The North- South line.")});
    }

    void runningHeadsAreLeftOutAndParagraphsKeptApart()
    {
        const QList<Sentence> found = sentencesOf({line(QStringLiteral("A Made-Up Journal, Volume 1"), 20, false, true),
                                                   line(QStringLiteral("A Heading Without A Stop"), 100, true),
                                                   line(QStringLiteral("The text after it."), 120, true),
                                                   line(QStringLiteral("[12]"), 140, true),
                                                   line(QStringLiteral("7"), 800, false, true)});
        QCOMPARE(texts(found), (QStringList{QStringLiteral("A Heading Without A Stop"), QStringLiteral("The text after it.")}));
    }

    void nothingToReadIsNothing()
    {
        QVERIFY(sentencesOf({}).isEmpty());
        QVERIFY(sentencesOf({line(QStringLiteral("   "), 100), line(QStringLiteral("— — —"), 112)}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SentencesTest)
#include "test_sentences.moc"
