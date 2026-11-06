// Parses untrusted files in its own process, so a crash can't take down the app.
// M0 placeholder: the job protocol arrives in M2.

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("papertrail-worker"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PAPERTRAIL_VERSION));

    QTextStream(stdout) << QCoreApplication::applicationName() << ' '
                        << QCoreApplication::applicationVersion() << '\n';
    return 0;
}
