// Asks the real online sources about one work, and prints what comes back. For checking that lookups
// work for real on each platform, including secure connections, since the tests never leave this computer.
//
//   papertrail-lookup --doi 10.1000/example
//   papertrail-lookup --isbn 9780000000000
//   papertrail-lookup --arxiv 2101.00001
//   papertrail-lookup --title "A title to search for"
//   papertrail-lookup --case "[2021] UKSC 5"            (a neutral citation, ECLI, EU case number or US report)
//   papertrail-lookup --case-name "A v B" --jurisdiction uk
//   papertrail-lookup --natural-voices FOLDER           (fetches the natural voices, 367 MB, as the app does)
//
// CourtListener and CanLII need keys of the user's own, read from PAPERTRAIL_COURTLISTENER_TOKEN and
// PAPERTRAIL_CANLII_KEY, never from the command line. Use public records only.

#include "core/model/PublicationFields.h"
#include "metadata/CaseLaw.h"
#include "metadata/Matching.h"
#include "metadata/NaturalVoices.h"
#include "metadata/Providers.h"
#include "metadata/Transport.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTextStream>
#include <QUrlQuery>

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

TransportReply fetchNow(Transport &transport, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers = {})
{
    TransportReply result;
    QEventLoop loop;
    getWithHeaders(transport, url, headers, [&](const TransportReply &reply) {
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
    parser.addOption({QStringLiteral("case"), QStringLiteral("A case's neutral citation, ECLI, EU case number or US report."),
                      QStringLiteral("citation")});
    parser.addOption({QStringLiteral("case-name"), QStringLiteral("A case's name to search for."), QStringLiteral("name")});
    parser.addOption({QStringLiteral("jurisdiction"), QStringLiteral("uk, us, eu or ca, for a case's name alone."),
                      QStringLiteral("where")});
    parser.addOption({QStringLiteral("natural-voices"),
                      QStringLiteral("Fetches the natural voices into this folder, as the app does, checking every file."),
                      QStringLiteral("folder")});
    parser.process(app);

    out << "Secure connections available: " << (NetworkTransport::canUseTls() ? "yes" : "NO") << '\n';
    out.flush();

    const ProviderUrls urls;
    NetworkTransport transport;
    bool allOk = true;
    const auto ask = [&](const QString &label, const QUrl &url, const std::function<QList<FoundRecord>(const QByteArray &)> &parse,
                         const QList<QPair<QByteArray, QByteArray>> &headers = {}) {
        out << label << ": ";
        out.flush();
        const TransportReply reply = fetchNow(transport, url, headers);
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
    if (parser.isSet(QStringLiteral("case")) || parser.isSet(QStringLiteral("case-name"))) {
        core::Publication p;
        p.type = core::PublicationType::CaseLaw;
        p.title = parser.value(QStringLiteral("case-name"));
        const QString citation = parser.value(QStringLiteral("case")).simplified();
        static const QRegularExpression report(QStringLiteral("^(\\d{1,5})\\s+(.+?)\\s+(\\d{1,5})$"));
        if (!celexForCaseNumber(citation).isEmpty() && euEcli(citation).isEmpty()) {
            p.caseNumber = citation;
        } else if (const QRegularExpressionMatch match = report.match(citation); match.hasMatch() && !canLiiCaseFor(citation)) {
            p.volume = match.captured(1);
            p.containerTitle = match.captured(2);
            p.pages = match.captured(3);
        } else {
            p.neutralCitation = citation;
        }
        const QString where = parser.value(QStringLiteral("jurisdiction"));
        CaseJurisdiction jurisdiction = caseJurisdictionOf(p);
        if (where == QLatin1StringView("uk"))
            jurisdiction = CaseJurisdiction::UnitedKingdom;
        else if (where == QLatin1StringView("us"))
            jurisdiction = CaseJurisdiction::UnitedStates;
        else if (where == QLatin1StringView("eu"))
            jurisdiction = CaseJurisdiction::EuropeanUnion;
        else if (where == QLatin1StringView("ca"))
            jurisdiction = CaseJurisdiction::Canada;

        switch (jurisdiction) {
        case CaseJurisdiction::UnitedKingdom:
            ask(QStringLiteral("Find Case Law"),
                findCaseLawSearchUrl(urls, p.neutralCitation.isEmpty() ? p.title : QStringLiteral("\"%1\"").arg(p.neutralCitation)),
                [](const QByteArray &b) { return parseFindCaseLaw(b); });
            break;
        case CaseJurisdiction::UnitedStates: {
            const auto headers = keyHeaders(kCourtListener, qEnvironmentVariable("PAPERTRAIL_COURTLISTENER_TOKEN"));
            if (headers.isEmpty()) {
                out << "CourtListener needs a token in PAPERTRAIL_COURTLISTENER_TOKEN.\n";
                allOk = false;
                break;
            }
            const QString cited = reporterCitationOf(p);
            ask(QStringLiteral("CourtListener"),
                cited.isEmpty() ? courtListenerNameUrl(urls, p.title, std::nullopt) : courtListenerCitationUrl(urls, cited),
                [](const QByteArray &b) { return parseCourtListenerSearch(b); }, headers);
            break;
        }
        case CaseJurisdiction::EuropeanUnion:
            ask(QStringLiteral("EUR-Lex"), eurLexQueryUrl(urls, euEcli(p.neutralCitation), celexForCaseNumber(p.caseNumber)),
                [](const QByteArray &b) { return parseEurLex(b); });
            break;
        case CaseJurisdiction::Canada: {
            const QString key = qEnvironmentVariable("PAPERTRAIL_CANLII_KEY");
            const auto which = canLiiCaseFor(p.neutralCitation);
            if (key.isEmpty() || !which) {
                out << "CanLII needs a key in PAPERTRAIL_CANLII_KEY, and a Canadian neutral citation.\n";
                allOk = false;
                break;
            }
            QUrl url = canLiiCaseUrl(urls, *which);
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("api_key"), key);
            url.setQuery(query);
            ask(QStringLiteral("CanLII"), url, [&](const QByteArray &b) { return one(parseCanLii(b)); },
                {{QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}});
            break;
        }
        case CaseJurisdiction::Unknown:
            out << "No source covers this case. Give --jurisdiction for a name alone.\n";
            allOk = false;
            break;
        }
    }
    if (parser.isSet(QStringLiteral("natural-voices"))) {
        const QString folder = parser.value(QStringLiteral("natural-voices"));
        out << "Natural voices from Hugging Face, " << naturalVoicesBytes() / 1000000 << " MB: ";
        out.flush();
        QEventLoop loop;
        QString error = QStringLiteral("never answered");
        qint64 shown = 0;
        downloadNaturalVoices(
            transport, folder,
            [&](qint64 received) {
                if (received - shown >= 50000000) {
                    shown = received;
                    out << received / 1000000 << " MB... ";
                    out.flush();
                }
            },
            [&](const QString &result) {
                error = result;
                loop.quit();
            });
        loop.exec();
        if (error.isEmpty() && naturalVoicesPresent(folder)) {
            out << "all there, in " << QDir::toNativeSeparators(naturalVoicesFolder(folder)) << '\n';
        } else {
            out << "FAILED: " << error << '\n';
            allOk = false;
        }
    }
    return allOk ? 0 : 1;
}
