// Splitting a page's lines into sentences to read aloud, and keeping where each is.

#include "core/text/Sentences.h"

#include <QTest>

using namespace papertrail::core;

namespace {

// A line at this height, from this far across, each character 6 points wide.
SentenceLine line(const QString &text, double y, bool paragraph = false, bool margin = false, double x = 50)
{
    SentenceLine made;
    made.text = text;
    for (qsizetype i = 0; i < text.size(); ++i)
        made.chars << QRectF(x + 6 * i, y, 6, 10);
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
    void aSentenceCarriedOverAPageIsJoined()
    {
        QVERIFY(endsSentence(QStringLiteral("It ends here.")));
        QVERIFY(endsSentence(QStringLiteral("Does it end? ")));
        QVERIFY(endsSentence(QStringLiteral("It ends (as this one does.)")));
        QVERIFY(endsSentence(QStringLiteral("\u201cIt ends here.\u201d")));
        QVERIFY(endsSentence(QStringLiteral("And so on\u2026")));
        QVERIFY(!endsSentence(QStringLiteral("these studies did not distinguish")));
        QVERIFY(!endsSentence(QStringLiteral("as follows:")));
        QVERIFY(!endsSentence(QString()));
        QCOMPARE(joinedAcrossPages(QStringLiteral("it carries on "), QStringLiteral(" over the page.")),
                 QStringLiteral("it carries on over the page."));
        QCOMPARE(joinedAcrossPages(QStringLiteral("broken in exam-"), QStringLiteral("ple of one.")),
                 QStringLiteral("broken in example of one."));
        QCOMPARE(joinedAcrossPages(QStringLiteral("a well-"), QStringLiteral("Known name.")), QStringLiteral("a well- Known name."));
    }

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

    void aRowInPiecesIsOneParagraph()
    {
        // A heading's number and words, which a file can keep as lines of their own, each seeming to start a paragraph.
        const QList<Sentence> found = sentencesOf({line(QStringLiteral("2.3"), 100, true),
                                                   line(QStringLiteral("Made-up measures"), 100, true, false, 110),
                                                   line(QStringLiteral("The text after it."), 120, true)});
        QCOMPARE(texts(found), (QStringList{QStringLiteral("2.3 Made-up measures"), QStringLiteral("The text after it.")}));
        QCOMPARE(found.first().rects.size(), qsizetype(2));
    }

    void abbreviationsAreSaidInFull()
    {
        QCOMPARE(spokenForm(QStringLiteral("As Fig. 3 and Eq. (4) show, e.g. in the appendix.")),
                 QStringLiteral("As Figure 3 and Equation (4) show, for example in the appendix."));
        QCOMPARE(spokenForm(QStringLiteral("See pp. 113-118, i.e. the last part, cf. p. 12.")),
                 QStringLiteral("See pages 113 to 118, that is the last part, compare page 12."));
        QCOMPARE(spokenForm(QStringLiteral("Smith et al. found it [12, 14\u201316] in 2019\u20132020 [3].")),
                 QStringLiteral("Smith and others found it in 2019 to 2020."));
        QCOMPARE(spokenForm(QStringLiteral("Figs. 2 and 3 compare model A vs model B.")),
                 QStringLiteral("Figures 2 and 3 compare model A versus model B."));
        // Words that only look like them are left alone.
        QCOMPARE(spokenForm(QStringLiteral("The p. value and the shape of a pp. hull.")),
                 QStringLiteral("The p. value and the shape of a pp. hull."));
    }

    void nothingToReadIsNothing()
    {
        QVERIFY(sentencesOf({}).isEmpty());
        QVERIFY(sentencesOf({line(QStringLiteral("   "), 100), line(QStringLiteral("— — —"), 112)}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SentencesTest)
#include "test_sentences.moc"
