#include "citation/CslEngine.h"
#include "citation/CslStyles.h"
#include "citation/Styles.h"
#include "CitationSamples.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QThread>
#include <QtTest>

#include <functional>
#include <optional>

using namespace papertrail;
using namespace papertrail::citation;
using tests::fixtureItems;

namespace {

const QDate kAccessed(2026, 11, 7);
const QString kStyles = QStringLiteral("http://www.zotero.org/styles/");
const QString kMadeUp = QStringLiteral("http://example.org/styles/");

QByteArray testStyle(const QString &name)
{
    QFile file(QStringLiteral(PAPERTRAIL_TEST_FIXTURES "/csl/styles/") + name + QStringLiteral(".csl"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The made-up styles, as if they'd been added to the library.
QByteArray addedStyle(const QString &id)
{
    return id.startsWith(kMadeUp) ? testStyle(id.mid(kMadeUp.size())) : QByteArray();
}

// Runs `work` on a thread with the stack CslEngine needs, and returns what it returns.
template <typename Work>
auto onDeepStack(Work work) -> decltype(work())
{
    std::optional<decltype(work())> result;
    std::unique_ptr<QThread> thread(QThread::create([&] { result.emplace(work()); }));
    thread->setStackSize(CslEngine::kCslStackBytes);
    thread->start();
    thread->wait();
    return std::move(*result);
}

// Styling shown as the fixture files write it: *italic*, **bold**, {sc:small capitals}, ^superscript^, ~subscript~.
QString marked(const RichText &text)
{
    QString out;
    for (const RichText::Run &run : text.runs()) {
        QString part = run.text;
        if (run.italic)
            part = u'*' + part + u'*';
        if (run.bold)
            part = QStringLiteral("**") + part + QStringLiteral("**");
        if (run.smallCaps)
            part = QStringLiteral("{sc:") + part + u'}';
        if (run.position == RichText::Position::Superscript)
            part = u'^' + part + u'^';
        else if (run.position == RichText::Position::Subscript)
            part = u'~' + part + u'~';
        out += part;
    }
    return out.simplified();
}

struct Rendered {
    QString error;
    QList<RichText> entries;
    QList<int> order;
    RichText citation;
    QList<RichText> each; // Each item's own citation, in the order given.
};

struct Options {
    QString locale;
    std::chrono::milliseconds base{60000};
    std::chrono::milliseconds perItem{100};
    bool each = false;
};

Rendered renderWith(CslEngine &engine, const QString &id, const QList<CitationItem> &items, const Options &options)
{
    Rendered rendered;
    engine.setLimits(options.base, options.perItem);
    const std::optional<CslLayout> layout = resolveCslStyle(id, options.locale, &addedStyle, &rendered.error);
    if (!layout || !engine.useStyle(*layout, &rendered.error))
        return rendered;
    StyleOptions styleOptions;
    styleOptions.accessed = kAccessed;
    const std::optional<FormattedReferences> references = engine.references(items, styleOptions, &rendered.error);
    if (!references)
        return rendered;
    rendered.entries = references->entries;
    rendered.order = references->order;
    const std::optional<RichText> citation = engine.citation(items, styleOptions, &rendered.error);
    if (!citation)
        return rendered;
    rendered.citation = *citation;
    if (options.each) {
        for (const CitationItem &item : items) {
            const std::optional<RichText> one = engine.citation({item}, styleOptions, &rendered.error);
            if (!one)
                return rendered;
            rendered.each << *one;
        }
    }
    return rendered;
}

Rendered render(const QString &id, const QList<CitationItem> &items, const Options &options = {})
{
    return onDeepStack([&] {
        CslEngine engine;
        return renderWith(engine, id, items, options);
    });
}

QList<CitationItem> allItems()
{
    QList<CitationItem> items;
    for (const auto &[name, item] : fixtureItems())
        items << item;
    return items;
}

} // namespace

class CslTest : public QObject {
    Q_OBJECT

private slots:
    void theBundledStylesAreAllThere()
    {
        const QList<CslStyleInfo> &styles = bundledCslStyles();
        QVERIFY2(styles.size() >= 34, qPrintable(QString::number(styles.size())));
        QStringList ids;
        for (const CslStyleInfo &style : styles) {
            QVERIFY(style.bundled);
            QVERIFY(!style.title.isEmpty());
            ids << style.id;
        }
        for (const QString &name : {QStringLiteral("apa"), QStringLiteral("vancouver-nlm"), QStringLiteral("nature"),
                                    QStringLiteral("modern-language-association"), QStringLiteral("chicago-note-bibliography"),
                                    QStringLiteral("american-medical-association"), QStringLiteral("oscola")})
            QVERIFY2(ids.contains(kStyles + name), qPrintable(name));
        // Every dependent style's parent is here too.
        for (const CslStyleInfo &style : styles) {
            if (style.isDependent())
                QVERIFY2(ids.contains(style.parentId), qPrintable(style.id));
        }
        QVERIFY(bundledCslStyle(QStringLiteral("http://example.org/nothing")).isEmpty());
    }

    void localesAreFoundByCodeOrLanguage()
    {
        QVERIFY(cslLocales().size() > 50);
        QVERIFY(cslLocale(QStringLiteral("en-US")).contains("xml:lang=\"en-US\""));
        QVERIFY(cslLocale(QStringLiteral("de")).contains("xml:lang=\"de-DE\""));
        QVERIFY(cslLocale(QStringLiteral("pt-XX")).contains("xml:lang=\"pt-"));
        // Nothing that could name another file.
        QVERIFY(cslLocale(QStringLiteral("../styles/apa")).isEmpty());
        QVERIFY(cslLocale(QStringLiteral("en-US/../x")).isEmpty());
        QVERIFY(cslLocale(QString()).isEmpty());
        QVERIFY(cslLocale(QStringLiteral("zz")).isEmpty());
    }

    void stylesAreCheckedBeforeUse()
    {
        QString error;
        const std::optional<CslStyleInfo> tiny = readCslStyle(testStyle(QStringLiteral("tiny")), &error);
        QVERIFY2(tiny, qPrintable(error));
        QCOMPARE(tiny->id, kMadeUp + QStringLiteral("tiny"));
        QCOMPARE(tiny->title, QStringLiteral("Made-up Tiny Style"));
        QVERIFY(tiny->hasBibliography);
        QVERIFY(!tiny->isNote);
        const std::optional<CslStyleInfo> dependent = readCslStyle(testStyle(QStringLiteral("dependent-on-tiny")));
        QVERIFY(dependent);
        QCOMPARE(dependent->parentId, kMadeUp + QStringLiteral("tiny"));
        QCOMPARE(dependent->defaultLocale, QStringLiteral("de-DE"));

        // Each of these is refused with a reason, rather than given to citeproc-js, which would render some of them as
        // nothing without complaint.
        for (const QString &name : {QStringLiteral("half"), QStringLiteral("no-layout"), QStringLiteral("dtd"),
                                    QStringLiteral("not-a-style")}) {
            error.clear();
            QVERIFY2(!readCslStyle(testStyle(name), &error), qPrintable(name));
            QVERIFY2(!error.isEmpty(), qPrintable(name));
        }
        QVERIFY(!readCslStyle(QByteArray()));
        QVERIFY(!readCslStyle(QByteArray(kMaxCslStyleBytes + 1, ' ')));
        const CslStyleInfo nature = *readCslStyle(bundledCslStyle(kStyles + QStringLiteral("nature")));
        QCOMPARE(nature.citationFormat, QStringLiteral("numeric"));
        const CslStyleInfo chicago = *readCslStyle(bundledCslStyle(kStyles + QStringLiteral("chicago-note-bibliography")));
        QVERIFY(chicago.isNote);
    }

    void markedTextBecomesRuns()
    {
        RichText margin;
        const QString text = QString(QChar(0xE002)) + QStringLiteral("[1]") + QChar(0xE003) + QStringLiteral("Plain ") + QChar(0xE000)
            + u'i' + QStringLiteral("italic ") + QChar(0xE000) + u'b' + QStringLiteral("both") + QChar(0xE001) + QChar(0xE001)
            + QStringLiteral(" x") + QChar(0xE000) + u'u' + QStringLiteral("2") + QChar(0xE001) + QChar(0xE000) + u'c'
            + QStringLiteral("Caps") + QChar(0xE001) + QChar(0xE001);
        const RichText body = CslEngine::fromMarked(text, &margin);
        QCOMPARE(margin.plain(), QStringLiteral("[1]"));
        QCOMPARE(marked(body), QStringLiteral("Plain *italic ****both*** x^2^{sc:Caps}"));
        QCOMPARE(body.plain(), QStringLiteral("Plain italic both x2Caps"));
        QCOMPARE(body.html(), QStringLiteral("Plain <i>italic </i><b><i>both</i></b> x<sup>2</sup>"
                                             "<span style=\"font-variant:small-caps\">Caps</span>"));
        // Stray closers and markers it doesn't use change nothing.
        QCOMPARE(CslEngine::fromMarked(QString(QChar(0xE001)) + QStringLiteral("a") + QChar(0xE0FF)).plain(), QStringLiteral("a"));
    }

    void aStyleOfOnesOwnIsRendered()
    {
        const QList<CitationItem> items = allItems();
        const Rendered rendered = render(kMadeUp + QStringLiteral("tiny"), items.mid(0, 2));
        QVERIFY2(rendered.error.isEmpty(), qPrintable(rendered.error));
        QCOMPARE(rendered.entries.size(), 2);
        // Sorted by title, as the style says, with its italics, superscript and bold.
        QCOMPARE(rendered.order, (QList<int>{0, 1}));
        const QString article = marked(rendered.entries.at(0));
        QVERIFY2(article.startsWith(QStringLiteral("Ada Beatrice Example and Ben Sample.")), qPrintable(article));
        QVERIFY2(article.contains(QStringLiteral("*Lattice Vibrations in Pretend Crystals*.")), qPrintable(article));
        QVERIFY2(article.endsWith(QStringLiteral("^3^ **43–70**")), qPrintable(article));
        QVERIFY2(rendered.citation.plain().startsWith(u'(') && rendered.citation.plain().contains(QStringLiteral("2021")),
                 qPrintable(rendered.citation.plain()));
    }

    void aDependentStyleUsesItsParentsLayout()
    {
        const QList<CitationItem> items = allItems().mid(0, 1);
        const Rendered rendered = render(kMadeUp + QStringLiteral("dependent-on-tiny"), items);
        QVERIFY2(rendered.error.isEmpty(), qPrintable(rendered.error));
        QVERIFY(rendered.entries.first().plain().contains(QStringLiteral("Lattice Vibrations")));
        // Its own default locale, German, joins two authors.
        QVERIFY2(rendered.entries.first().plain().contains(QStringLiteral(" und ")), qPrintable(rendered.entries.first().plain()));

        // One whose parent isn't here says to add it, and nothing is fetched.
        QString error;
        QVERIFY(!resolveCslStyle(kMadeUp + QStringLiteral("orphan"), QString(), &addedStyle, &error));
        QVERIFY2(error.contains(QStringLiteral("Add its parent style")), qPrintable(error));
        QVERIFY(!resolveCslStyle(kMadeUp + QStringLiteral("not-here"), QString(), &addedStyle, &error));

        // Vancouver comes with Papertrail as a dependent style, with its parent.
        const Rendered vancouver = render(kStyles + QStringLiteral("vancouver-nlm"), allItems().mid(0, 2));
        QVERIFY2(vancouver.error.isEmpty(), qPrintable(vancouver.error));
        QVERIFY2(vancouver.entries.first().plain().startsWith(QStringLiteral("1.")), qPrintable(vancouver.entries.first().plain()));
    }

    void theLocaleCanBeChosen()
    {
        const QList<CitationItem> items = allItems().mid(0, 1);
        const Rendered english = render(kStyles + QStringLiteral("modern-language-association"), items);
        QVERIFY2(english.error.isEmpty(), qPrintable(english.error));
        QVERIFY2(english.entries.first().plain().contains(QStringLiteral(" and ")), qPrintable(english.entries.first().plain()));
        Options german;
        german.locale = QStringLiteral("de-DE");
        const Rendered deutsch = render(kStyles + QStringLiteral("modern-language-association"), items, german);
        QVERIFY2(deutsch.error.isEmpty(), qPrintable(deutsch.error));
        QVERIFY2(deutsch.entries.first().plain().contains(QStringLiteral(" und ")), qPrintable(deutsch.entries.first().plain()));
    }

    void everyBundledStyleRendersEveryKind()
    {
        const QList<CitationItem> items = allItems();
        const QStringList problems = onDeepStack([&] {
            QStringList found;
            CslEngine engine;
            for (const CslStyleInfo &style : bundledCslStyles()) {
                QElapsedTimer timer;
                timer.start();
                const Rendered rendered = renderWith(engine, style.id, items, {});
                if (!rendered.error.isEmpty()) {
                    found << style.id + QStringLiteral(": ") + rendered.error;
                    continue;
                }
                if (rendered.entries.size() != items.size())
                    found << style.id + QStringLiteral(": %1 entries").arg(rendered.entries.size());
                for (const RichText &entry : rendered.entries) {
                    if (entry.isEmpty() || entry.plain().contains(QStringLiteral("NO_PRINTED_FORM")))
                        found << style.id + QStringLiteral(": an empty entry");
                }
                if (rendered.citation.isEmpty())
                    found << style.id + QStringLiteral(": no citation");
                qInfo("%s: %lld ms", qPrintable(style.id.mid(kStyles.size())), timer.elapsed());
            }
            return found;
        });
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(u'\n')));
    }

    void fiveStylesKeepTheirOutput_data()
    {
        QTest::addColumn<QString>("name");
        for (const char *name : {"vancouver-nlm", "nature", "american-medical-association", "modern-language-association",
                                 "chicago-note-bibliography"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void fiveStylesKeepTheirOutput()
    {
        QFETCH(QString, name);
        const QList<std::pair<QString, CitationItem>> fixtures = fixtureItems();
        QList<CitationItem> items;
        for (const auto &[key, item] : fixtures)
            items << item;
        Options options;
        options.each = true;
        const Rendered rendered = render(kStyles + name, items, options);
        QVERIFY2(rendered.error.isEmpty(), qPrintable(rendered.error));
        QCOMPARE(rendered.entries.size(), items.size());

        // Each kind's entry and citation, by the names test_citations uses, in the style's order.
        QString text;
        for (qsizetype i = 0; i < rendered.entries.size(); ++i) {
            const int index = rendered.order.at(i);
            text += u'[' + fixtures.at(index).first + QStringLiteral("]\nref: ") + marked(rendered.entries.at(i))
                + QStringLiteral("\ncite: ") + marked(rendered.each.at(index)) + QStringLiteral("\n\n");
        }
        text += QStringLiteral("[all]\ncite: ") + marked(rendered.citation) + u'\n';

        const QString path = QStringLiteral(PAPERTRAIL_TEST_FIXTURES "/csl/") + name + QStringLiteral(".txt");
        if (qEnvironmentVariableIsSet("PAPERTRAIL_WRITE_CSL_FIXTURES")) {
            QFile out(path);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(text.toUtf8());
            QSKIP("Written, to be checked by eye.");
        }
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const QStringList expected = QString::fromUtf8(file.readAll()).split(u'\n');
        const QStringList actual = text.split(u'\n');
        for (qsizetype i = 0; i < qMax(expected.size(), actual.size()); ++i)
            QCOMPARE(actual.value(i), expected.value(i));
    }

    void casesKeepTheirLegalForm()
    {
        QList<CitationItem> cases;
        for (const auto &[name, item] : fixtureItems()) {
            if (item.kind == CitationItem::Kind::Case)
                cases << item;
        }
        QVERIFY(cases.size() >= 2);
        for (const QString &name : {QStringLiteral("nature"), QStringLiteral("apa"), QStringLiteral("modern-language-association")}) {
            const Rendered rendered = render(kStyles + name, cases);
            QVERIFY2(rendered.error.isEmpty(), qPrintable(rendered.error));
            for (qsizetype i = 0; i < rendered.entries.size(); ++i) {
                const QString legal = caseCitation(cases.at(rendered.order.at(i))).plain();
                QVERIFY2(rendered.entries.at(i).plain().endsWith(legal), qPrintable(name + u' ' + rendered.entries.at(i).plain()));
            }
        }
        // A numeric style keeps its number.
        const Rendered nature = render(kStyles + QStringLiteral("nature"), cases);
        QVERIFY(nature.entries.first().plain().startsWith(QStringLiteral("1.")));
    }

    void stylesThatLoopOrRunAwayAreStopped()
    {
        const QList<CitationItem> items = allItems().mid(0, 2);
        Options quick;
        quick.base = std::chrono::milliseconds(3000);
        quick.perItem = std::chrono::milliseconds(0);
        QElapsedTimer timer;
        timer.start();
        const Rendered recursive = render(kMadeUp + QStringLiteral("recursive"), items, quick);
        QVERIFY(!recursive.error.isEmpty());
        const Rendered fanOut = render(kMadeUp + QStringLiteral("fan-out"), items, quick);
        QVERIFY(!fanOut.error.isEmpty());
        QVERIFY2(timer.elapsed() < 20000, qPrintable(QString::number(timer.elapsed())));

        // The same engine goes on to work after a style it had to stop.
        const QStringList after = onDeepStack([&] {
            CslEngine engine;
            QStringList errors;
            errors << renderWith(engine, kMadeUp + QStringLiteral("fan-out"), items, quick).error;
            errors << renderWith(engine, kMadeUp + QStringLiteral("tiny"), items, quick).error;
            return errors;
        });
        QVERIFY(!after.at(0).isEmpty());
        QVERIFY2(after.at(1).isEmpty(), qPrintable(after.at(1)));
    }

    void aNewerRequestCanStopAnOlderOne()
    {
        const QString error = onDeepStack([] {
            CslEngine engine;
            auto cancelled = std::make_shared<std::atomic_bool>(false);
            engine.setCancelFlag(cancelled);
            QString why;
            const std::optional<CslLayout> layout = resolveCslStyle(kStyles + QStringLiteral("nature"), QString(), {}, &why);
            if (!layout || !engine.useStyle(*layout, &why))
                return QStringLiteral("not ready: ") + why;
            cancelled->store(true);
            if (engine.references(allItems(), {}, &why))
                return QStringLiteral("not stopped");
            cancelled->store(false);
            if (!engine.useStyle(*layout, &why) || !engine.references(allItems(), {}, &why))
                return QStringLiteral("not working afterwards: ") + why;
            return QString();
        });
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void damagedStylesNeverHangOrCrash()
    {
        const int rounds = qEnvironmentVariableIntValue("PAPERTRAIL_DAMAGE_ROUNDS") > 0 ? qEnvironmentVariableIntValue("PAPERTRAIL_DAMAGE_ROUNDS") : 8;
        const quint32 seed = qEnvironmentVariableIsSet("PAPERTRAIL_DAMAGE_SEED") ? quint32(qEnvironmentVariableIntValue("PAPERTRAIL_DAMAGE_SEED")) : 16;
        const QList<CitationItem> items = allItems();
        const int finished = onDeepStack([&] {
            QRandomGenerator random(seed);
            CslEngine engine;
            engine.setLimits(std::chrono::milliseconds(3000), std::chrono::milliseconds(0));
            int done = 0;
            for (const QString &name : {QStringLiteral("nature"), QStringLiteral("ieee"), QStringLiteral("nlm-citation-sequence")}) {
                const QByteArray original = bundledCslStyle(kStyles + name);
                for (int round = 0; round < rounds; ++round) {
                    QByteArray damaged = original;
                    const int changes = 1 + int(random.bounded(8));
                    for (int change = 0; change < changes && !damaged.isEmpty(); ++change) {
                        const qsizetype at = random.bounded(int(damaged.size()));
                        switch (random.bounded(3)) {
                        case 0: damaged[at] = char(random.bounded(256)); break;
                        case 1: damaged.remove(at, 1 + random.bounded(200)); break;
                        default: damaged.insert(at, damaged.mid(random.bounded(int(damaged.size())), 1 + random.bounded(300))); break;
                        }
                    }
                    const std::optional<CslStyleInfo> info = readCslStyle(damaged);
                    if (info && !info->isDependent()) {
                        CslLayout layout;
                        layout.style = *info;
                        layout.xml = damaged;
                        layout.locale = QStringLiteral("en-US");
                        if (engine.useStyle(layout)) {
                            engine.references(items);
                            engine.citation(items);
                        }
                    }
                    ++done;
                }
            }
            return done;
        });
        QCOMPARE(finished, 3 * rounds);
    }
};

QTEST_GUILESS_MAIN(CslTest)
#include "test_csl.moc"
