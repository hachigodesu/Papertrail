#pragma once

#include "metadata/Providers.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

// The online sources for court judgments, which know nothing of DOIs and ISBNs, and how a case is matched against what
// they say. Like Providers, everything here is plain functions over text, and nothing in an answer is trusted.
namespace papertrail::metadata {

extern const QString kFindCaseLaw;   // The National Archives' Find Case Law, for the UK.
extern const QString kCourtListener; // The Free Law Project's CourtListener, for the US. Needs the user's own token.
extern const QString kEurLex;        // The EU Publications Office's CELLAR, behind EUR-Lex, for the EU's courts.
extern const QString kCanLii;        // CanLII, for Canada. Needs the user's own key.

// Which jurisdiction's source could say something about a case, from its neutral citation or ECLI first, then its case
// number, jurisdiction, court, law report and citation form. Unknown for anywhere no free source covers, such as
// Scotland, Northern Ireland, Ireland, Australia and New Zealand.
enum class CaseJurisdiction { Unknown, UnitedKingdom, UnitedStates, EuropeanUnion, Canada };
CaseJurisdiction caseJurisdictionOf(const core::Publication &publication);

// ---- Find Case Law

// Whether a neutral citation is from a court or tribunal Find Case Law holds: the UK's, and England and Wales'.
bool isFindCaseLawCitation(const QString &citation);
// A search of its feed, most relevant first.
QUrl findCaseLawSearchUrl(const ProviderUrls &urls, const QString &query);
QList<FoundRecord> parseFindCaseLaw(const QByteArray &body);

// ---- CourtListener

// A case's law report as one citation, such as "410 U.S. 113", from its container, volume and first page. Empty if it
// hasn't all three.
QString reporterCitationOf(const core::Publication &publication);
QUrl courtListenerCitationUrl(const ProviderUrls &urls, const QString &reporterCitation);
QUrl courtListenerNameUrl(const ProviderUrls &urls, const QString &name, std::optional<int> year);
QList<FoundRecord> parseCourtListenerSearch(const QByteArray &body);

// ---- EUR-Lex

// "ECLI:EU:C:2014:317" in a standard form, or empty if it isn't an EU court's ECLI.
QString euEcli(const QString &text);
// The CELEX number of a judgment's case number, such as "62012CJ0131" for "C-131/12" or "Case T-12/20". Empty if it
// isn't one.
QString celexForCaseNumber(const QString &caseNumber);
// A fixed query, asking by ECLI if there's one, or else by CELEX number. Empty if neither is in a form trusted.
QUrl eurLexQueryUrl(const ProviderUrls &urls, const QString &ecli, const QString &celex);
QList<FoundRecord> parseEurLex(const QByteArray &body);

// ---- CanLII

// The database and case CanLII files a Canadian neutral citation under, such as "csc-scc" and "2022scc30" for
// "2022 SCC 30".
struct CanLiiCase {
    QString databaseId;
    QString caseId;
};
std::optional<CanLiiCase> canLiiCaseFor(const QString &neutralCitation);
// Without the key, which is added only as it's sent.
QUrl canLiiCaseUrl(const ProviderUrls &urls, const CanLiiCase &which);
std::optional<FoundRecord> parseCanLii(const QByteArray &body);

// ---- Matching

// From 0 to 1, as titleSimilarity, with "v", "v." and "vs" alike, "R", "Regina" and "The Queen" alike, and words
// such as "and others" and "Ltd" left out.
double caseNameSimilarity(const QString &a, const QString &b);
// How likely a search result is to be this case, by its name, year and court. Cases have no authors to go by.
double caseMatchScore(const core::Publication &publication, const FoundRecord &found);
// How `found` has the same citation as the case: "neutral_citation", "ecli", "case_number" or "report". Empty if it
// hasn't.
QString sameCaseCitation(const core::Publication &publication, const FoundRecord &found);
// Whether what a citation led to is this case, by its name, or a file's name. A case with no name agrees.
bool caseNameAgrees(const core::Publication &publication, const QStringList &fileNames, const FoundRecord &found);

} // namespace papertrail::metadata
