// Reading references in a document's text, and recognising what they point at. Everything here is made up.

#include "core/text/References.h"

#include <QtTest>

using namespace papertrail::core;
using Kind = TextReference::Kind;

class ReferencesTest : public QObject {
    Q_OBJECT

    // The reference at the first place `at` appears in `text`, plus `into` characters.
    static std::optional<TextReference> at(const QString &text, const QString &where, int into = 0)
    {
        const qsizetype index = text.indexOf(where);
        if (index < 0)
            qFatal("not in the text");
        return referenceAt(text, int(index) + into);
    }

private slots:
    void readsFiguresTablesAndTheirKin()
    {
        const QString text = QStringLiteral("As Figure 3 shows, and Fig. 4b too, the values in Table 2 and Tab. 5 agree.");
        auto found = at(text, QStringLiteral("Figure 3"));
        QVERIFY(found);
        QCOMPARE(found->kind, Kind::Figure);
        QCOMPARE(found->number, QStringLiteral("3"));
        QCOMPARE(found->label(), QStringLiteral("Figure 3"));
        // On its number, too.
        QCOMPARE(at(text, QStringLiteral("3 shows"))->number, QStringLiteral("3"));
        found = at(text, QStringLiteral("Fig. 4b"), 2);
        QCOMPARE(found->kind, Kind::Figure);
        QCOMPARE(found->number, QStringLiteral("4b"));
        found = at(text, QStringLiteral("Table 2"));
        QCOMPARE(found->kind, Kind::Table);
        QCOMPARE(found->number, QStringLiteral("2"));
        QCOMPARE(at(text, QStringLiteral("Tab. 5"))->number, QStringLiteral("5"));

        // Of several, the one clicked, or the first when it's the word.
        const QString several = QStringLiteral("See Figs. 3 and 4, and Tables 1–3.");
        QCOMPARE(at(several, QStringLiteral("4,"))->number, QStringLiteral("4"));
        QCOMPARE(at(several, QStringLiteral("Figs."))->number, QStringLiteral("3"));
        QCOMPARE(at(several, QStringLiteral("3."))->number, QStringLiteral("3"));
        QCOMPARE(at(several, QStringLiteral("3."))->kind, Kind::Table);

        const QString maths = QStringLiteral("By Lemma 2 and Theorem 1.3, with Definition 4, as in Section 2.1 and Appendix B.");
        found = at(maths, QStringLiteral("Lemma"));
        QCOMPARE(found->kind, Kind::Theorem);
        QCOMPARE(found->word, QStringLiteral("Lemma"));
        QCOMPARE(at(maths, QStringLiteral("Theorem"))->number, QStringLiteral("1.3"));
        QCOMPARE(at(maths, QStringLiteral("Definition"))->label(), QStringLiteral("Definition 4"));
        QCOMPARE(at(maths, QStringLiteral("Section"))->number, QStringLiteral("2.1"));
        found = at(maths, QStringLiteral("Appendix"));
        QCOMPARE(found->kind, Kind::Appendix);
        QCOMPARE(found->number, QStringLiteral("B"));

        // A word that's only a word isn't a reference.
        QVERIFY(!at(QStringLiteral("The table and the figure agree."), QStringLiteral("table")));
        QVERIFY(!at(QStringLiteral("Nothing here."), QStringLiteral("here")));
    }

    void readsEquations()
    {
        const QString text = QStringLiteral("From Eq. (4) and Equation 5, then (6) follows, as in eqs. (7)–(8).");
        auto found = at(text, QStringLiteral("Eq."));
        QCOMPARE(found->kind, Kind::Equation);
        QCOMPARE(found->number, QStringLiteral("4"));
        QCOMPARE(found->label(), QStringLiteral("Equation 4"));
        QCOMPARE(at(text, QStringLiteral("Equation 5"))->number, QStringLiteral("5"));
        found = at(text, QStringLiteral("(6)"), 1);
        QCOMPARE(found->kind, Kind::Equation);
        QCOMPARE(found->number, QStringLiteral("6"));
        QCOMPARE(at(text, QStringLiteral("(8)"), 1)->number, QStringLiteral("8"));
    }

    void readsCitations()
    {
        const QString numbered = QStringLiteral("Earlier work [4] and others [3, 7–9] found the same.");
        auto found = at(numbered, QStringLiteral("[4]"), 1);
        QCOMPARE(found->kind, Kind::Citation);
        QCOMPARE(found->number, QStringLiteral("4"));
        QCOMPARE(found->label(), QStringLiteral("reference 4"));
        QCOMPARE(at(numbered, QStringLiteral("7–9"))->number, QStringLiteral("7"));
        QCOMPARE(at(numbered, QStringLiteral("9]"))->number, QStringLiteral("9"));
        QCOMPARE(at(numbered, QStringLiteral("[3"))->number, QStringLiteral("3"));

        const QString named = QStringLiteral("As Garner and Holt (2019) found, and later (Lane et al., 2020a; Moss 2018), "
                                             "the bending of Ödegaard (2001) was right. In 2019 nothing changed.");
        found = at(named, QStringLiteral("Garner"));
        QCOMPARE(found->kind, Kind::AuthorYear);
        QCOMPARE(found->surname, QStringLiteral("Garner"));
        QCOMPARE(found->year, QStringLiteral("2019"));
        QCOMPARE(found->label(), QStringLiteral("Garner 2019"));
        QCOMPARE(at(named, QStringLiteral("2019)"))->surname, QStringLiteral("Garner"));
        found = at(named, QStringLiteral("Lane"));
        QCOMPARE(found->surname, QStringLiteral("Lane"));
        QCOMPARE(found->year, QStringLiteral("2020a"));
        QCOMPARE(at(named, QStringLiteral("Moss"))->year, QStringLiteral("2018"));
        QCOMPARE(at(named, QStringLiteral("Ödegaard"))->surname, QStringLiteral("Ödegaard"));
        // A year after a word that isn't a name isn't a citation.
        QVERIFY(!at(named, QStringLiteral("In 2019")));
    }

    void aReferenceSplitOverLinesIsReadWhole()
    {
        // The worker joins the line before and after with spaces.
        const QString joined = QStringLiteral("the result shown in Figure 2 of the made-up study");
        QCOMPARE(at(joined, QStringLiteral("2 of"))->number, QStringLiteral("2"));
        QVERIFY(!referenceAt(joined, -1));
        QVERIFY(!referenceAt(joined, int(joined.size())));
    }

    void captionsAndHeadingsAreToldFromMentions()
    {
        TextReference figure{Kind::Figure, QStringLiteral("Figure"), QStringLiteral("2"), {}, {}};
        QVERIFY(isTargetLine(figure, QStringLiteral("Figure 2: A made-up beam bending under load.")));
        QVERIFY(isTargetLine(figure, QStringLiteral("  FIGURE 2. The beam.")));
        QVERIFY(isTargetLine(figure, QStringLiteral("Fig. 2 | The beam")));
        QVERIFY(isTargetLine(figure, QStringLiteral("Fig. 2 The beam")));
        QVERIFY(isTargetLine(figure, QStringLiteral("Figure 2")));
        QVERIFY(!isTargetLine(figure, QStringLiteral("Figure 2 is drawn above.")));
        QVERIFY(!isTargetLine(figure, QStringLiteral("Figure 21: Another one.")));
        QVERIFY(!isTargetLine(figure, QStringLiteral("As Figure 2: shows")));

        TextReference table{Kind::Table, QStringLiteral("Table"), QStringLiteral("1"), {}, {}};
        QVERIFY(isTargetLine(table, QStringLiteral("Table 1: Made-up values for each beam.")));
        QVERIFY(!isTargetLine(figure, QStringLiteral("Table 2: Values.")));

        TextReference lemma{Kind::Theorem, QStringLiteral("Lemma"), QStringLiteral("3"), {}, {}};
        QVERIFY(isTargetLine(lemma, QStringLiteral("Lemma 3. Every made-up beam bends.")));
        QVERIFY(isTargetLine(lemma, QStringLiteral("Lemma 3 (Bending). Every beam bends.")));
        QVERIFY(!isTargetLine(lemma, QStringLiteral("Theorem 3. Something else.")));

        TextReference section{Kind::Section, QStringLiteral("Section"), QStringLiteral("2"), {}, {}};
        QVERIFY(isTargetLine(section, QStringLiteral("2 Method")));
        QVERIFY(isTargetLine(section, QStringLiteral("2. Method")));
        QVERIFY(isTargetLine(section, QStringLiteral("Section 2 Method")));
        QVERIFY(!isTargetLine(section, QStringLiteral("2.1 Method")));
        QVERIFY(!isTargetLine(section, QStringLiteral("2 beams were bent.")));

        TextReference appendix{Kind::Appendix, QStringLiteral("Appendix"), QStringLiteral("B"), {}, {}};
        QVERIFY(isTargetLine(appendix, QStringLiteral("Appendix B: Proofs")));
        QVERIFY(!isTargetLine(appendix, QStringLiteral("Appendix Bending")));

        QVERIFY(endsWithEquationNumber(QStringLiteral("F = k x   (3)"), QStringLiteral("3")));
        QVERIFY(endsWithEquationNumber(QStringLiteral("(3)"), QStringLiteral("3")));
        QVERIFY(!endsWithEquationNumber(QStringLiteral("F = k x (3) holds"), QStringLiteral("3")));
        QVERIFY(!endsWithEquationNumber(QStringLiteral("F = k x (13)"), QStringLiteral("3")));
    }

    void entriesInAListOfReferencesAreFound()
    {
        QVERIFY(isReferencesHeading(QStringLiteral("References")));
        QVERIFY(isReferencesHeading(QStringLiteral("  BIBLIOGRAPHY ")));
        QVERIFY(isReferencesHeading(QStringLiteral("7. References")));
        QVERIFY(isReferencesHeading(QStringLiteral("Works Cited")));
        QVERIFY(!isReferencesHeading(QStringLiteral("References to other work are rare.")));

        QVERIFY(startsNumberedEntry(QStringLiteral("[4] D. Example, More made-up beams, 2010."), QStringLiteral("4")));
        QVERIFY(startsNumberedEntry(QStringLiteral("4. D. Example, More made-up beams."), QStringLiteral("4")));
        QVERIFY(startsNumberedEntry(QStringLiteral("4 D. Example, More made-up beams."), QStringLiteral("4")));
        QVERIFY(startsNumberedEntry(QStringLiteral("[12] Someone."), QString()));
        QVERIFY(!startsNumberedEntry(QStringLiteral("[14] Someone."), QStringLiteral("4")));
        QVERIFY(!startsNumberedEntry(QStringLiteral("4 beams were bent."), QStringLiteral("4")));

        QVERIFY(startsAuthorEntry(QStringLiteral("Garner, P. and Holt, R. (2019) Bending."), QStringLiteral("Garner")));
        QVERIFY(startsAuthorEntry(QStringLiteral("[3] GARNER P, Holt R. Bending."), QStringLiteral("Garner")));
        QVERIFY(startsAuthorEntry(QStringLiteral("Odegaard, K. (2001)"), QStringLiteral("Ödegaard")));
        QVERIFY(!startsAuthorEntry(QStringLiteral("Garnerson, P. (2019)"), QStringLiteral("Garner")));
        QVERIFY(!startsAuthorEntry(QStringLiteral("Holt, R. and Garner, P."), QStringLiteral("Garner")));

        QVERIFY(entryHasYear(QStringLiteral("Lane, T. (2020a) Another study."), QStringLiteral("2020a")));
        QVERIFY(entryHasYear(QStringLiteral("Lane, T. 2020. Another study."), QStringLiteral("2020")));
        QVERIFY(!entryHasYear(QStringLiteral("Lane, T. (2020a) Another study."), QStringLiteral("2020")));
        QVERIFY(!entryHasYear(QStringLiteral("Lane, T. pages 12020."), QStringLiteral("2020")));
    }
};

QTEST_GUILESS_MAIN(ReferencesTest)
#include "test_references.moc"
