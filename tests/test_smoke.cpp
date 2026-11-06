#include "ui/MainWindow.h"

#include <QtTest>

class SmokeTest : public QObject {
    Q_OBJECT

private slots:
    void mainWindowOpens()
    {
        papertrail::ui::MainWindow window;
        window.show();

        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.windowTitle(), QStringLiteral("Papertrail"));
    }
};

QTEST_MAIN(SmokeTest)
#include "test_smoke.moc"
