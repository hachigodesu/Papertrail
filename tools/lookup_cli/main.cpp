// Asks the real online sources about one work, and prints what comes back. For checking that lookups
// work for real on each platform, including secure connections, since the tests never leave this computer.
//
//   papertrail-lookup --doi 10.1000/example
//   papertrail-lookup --isbn 9780000000000
//   papertrail-lookup --arxiv 2101.00001
//   papertrail-lookup --title "A title to search for"

#include "core/model/PublicationFields.h"
#include "metadata/Matching.h"
#include "metadata/Providers.h"
#include "metadata/Transport.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTextStream>

using namespace papertrail;
using namespace papertrail::metadata;

namespace {

void printRecord(QTextStream &out, const FoundRecord &record)
{
    out << "  from " << providerLabel(record.provider);
    if (record.type)
        out << ", type " << core::toDbString(*record.type);
    if (record.preprintHint)
        out << ", which calls it a preprint";
    out << '\n';
    for (const QString &name : core::metadataFieldNames()) {
        if (name == QLatin1StringView("type"))
            continue;
        const QString value = core::metadataFieldValue(record.publication, name);
        if (!value.isEmpty())
            out << "    " << name.leftJustified(16) << value.left(100) << '\n';
    }
}

TransportReply fetchNow(Transport &transport, const QUrl &url)
{
    TransportReply result;
    QEventLoop loop;
    transport.get(url, [&](const TransportReply &reply) {
        result = reply;
        loop.quit();
    });
    loop.exec();
    return result;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("doi"), QStringLiteral("A DOI to look up."), QStringLiteral("doi")});
    parser.addOption({QStringLiteral("isbn"), QStringLiteral("An ISBN to look up."), QStringLiteral("isbn")});
    parser.addOption({QStringLiteral("arxiv"), QStringLiteral("An arXiv number to look up."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("title"), QStringLiteral("A title to search for."), QStringLiteral("title")});
    parser.process(app);

    out << "Secure connections available: " << (NetworkTransport::canUseTls() ? "yes" : "NO") << '\n';
    out.flush();

    const ProviderUrls urls;
    NetworkTransport transport;
    bool allOk = true;
    const auto ask = [&](const QString &label, const QUrl &url, const std::function<QList<FoundRecord>(const QByteArray &)> &parse) {
        out << label << ": ";
        out.flush();
        const TransportReply reply = fetchNow(transport, url);
        if (!reply.error.isEmpty()) {
            out << "FAILED: " << reply.error << '\n';
            allOk = false;
            return;
        }
        out << "status " << reply.status << ", " << reply.body.size() << " bytes\n";
        if (reply.status != 200) {
            allOk = false;
            return;
        }
        const QList<FoundRecord> records = parse(reply.body);
        if (records.isEmpty())
            allOk = false;
        for (int i = 0; i < records.size() && i < 3; ++i)
            printRecord(out, records.at(i));
        out.flush();
    };
    const auto one = [](const std::optional<FoundRecord> &record) {
        return record ? QList<FoundRecord>{*record} : QList<FoundRecord>{};
    };

    if (parser.isSet(QStringLiteral("doi"))) {
        const QString doi = parser.value(QStringLiteral("doi"));
        ask(QStringLiteral("Crossref"), crossrefWorkUrl(urls, doi), [&](const QByteArray &b) { return one(parseCrossrefWork(b)); });
        ask(QStringLiteral("OpenAlex"), openAlexWorkUrl(urls, doi), [&](const QByteArray &b) { return one(parseOpenAlexWork(b)); });
    }
    if (parser.isSet(QStringLiteral("isbn"))) {
        ask(QStringLiteral("Open Library"), openLibraryUrl(urls, parser.value(QStringLiteral("isbn"))),
            [&](const QByteArray &b) { return one(parseOpenLibrary(b)); });
    }
    if (parser.isSet(QStringLiteral("arxiv"))) {
        ask(QStringLiteral("arXiv"), arxivUrl(urls, parser.value(QStringLiteral("arxiv"))),
            [&](const QByteArray &b) { return one(parseArxiv(b)); });
    }
    if (parser.isSet(QStringLiteral("title"))) {
        const QString title = parser.value(QStringLiteral("title"));
        ask(QStringLiteral("Crossref search"), crossrefSearchUrl(urls, title), [](const QByteArray &b) { return parseCrossrefSearch(b); });
        ask(QStringLiteral("OpenAlex search"), openAlexSearchUrl(urls, title), [](const QByteArray &b) { return parseOpenAlexSearch(b); });
    }
    return allOk ? 0 : 1;
}
