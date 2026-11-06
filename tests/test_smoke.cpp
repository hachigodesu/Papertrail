#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"

#include <QTemporaryDir>
#include <QtTest>

class SmokeTest : public QObject {
    Q_OBJECT

private slots:
    void mainWindowOpens()
    {
        QTemporaryDir dir;
        papertrail::ui::LibraryPaths paths;
        paths.database = dir.filePath(QStringLiteral("library.db"));
        paths.cacheDirectory = dir.filePath(QStringLiteral("cache"));
        papertrail::ui::LibrarySession session(paths);

        papertrail::ui::MainWindow window(session);
        window.show();

        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.windowTitle(), QStringLiteral("Papertrail"));
    }
};

QTEST_MAIN(SmokeTest)
#include "test_smoke.moc"
