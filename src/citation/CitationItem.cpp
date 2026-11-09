#include "citation/CitationItem.h"

#include "core/db/Database.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"

#include <QHash>

namespace papertrail::citation {

using core::Publication;
using core::PublicationType;
using Kind = CitationItem::Kind;

QString CitationItem::link() const
{
    if (!doi.isEmpty())
        return QStringLiteral("https://doi.org/") + doi;
    if (!url.isEmpty())
        return url;
    if (!arxivId.isEmpty())
        return QStringLiteral("https://arxiv.org/abs/") + arxivId;
    return {};
}

Kind kindFor(const Publication &publication)
{
    switch (publication.type) {
    case PublicationType::JournalArticle: return Kind::Article;
    case PublicationType::JournalIssue: return Kind::Issue;
    case PublicationType::ConferenceProceedings: return Kind::Proceedings;
    case PublicationType::ConferencePaper: return Kind::Paper;
    case PublicationType::Textbook:
    case PublicationType::Book:
        return publication.authors.isEmpty() && !publication.editors.isEmpty() ? Kind::EditedBook : Kind::Book;
    case PublicationType::BookChapter: return Kind::Chapter;
    case PublicationType::Thesis:
    case PublicationType::Dissertation: return Kind::Thesis;
    case PublicationType::TechnicalReport: return Kind::Report;
    case PublicationType::Standard: return Kind::Standard;
    case PublicationType::Preprint: return Kind::Preprint;
    case PublicationType::Other: return Kind::Document;
    case PublicationType::CaseLaw: return Kind::Case;
    }
    return Kind::Document;
}

bool lacksDetails(const CitationItem &item)
{
    // A case has no authors: it needs a year and something to find it by.
    if (item.kind == Kind::Case)
        return item.title.isEmpty() || !item.year
               || (item.containerTitle.isEmpty() && item.neutralCitation.isEmpty() && item.caseNumber.isEmpty());
    // A report or document with nobody named is cited under the organisation responsible for it.
    const bool organisation = (item.kind == Kind::Report || item.kind == Kind::Document)
        && (!item.institution.isEmpty() || !item.publisher.isEmpty());
    const bool nobody = item.authors.isEmpty() && item.editors.isEmpty() && item.standardBody.isEmpty() && !organisation;
    const bool notInAnything = (item.kind == Kind::Article || item.kind == Kind::Chapter || item.kind == Kind::Paper)
        && item.containerTitle.isEmpty();
    return nobody || !item.year || notInAnything;
}

namespace {

QString orFrom(const QString &own, const QString &fallback)
{
    return own.trimmed().isEmpty() ? fallback.trimmed() : own.trimmed();
}

} // namespace

CitationItem citationItem(const Publication &p, const std::optional<Publication> &holder, const QStringList &keywords)
{
    CitationItem item;
    item.kind = kindFor(p);
    item.type = p.type;
    item.uuid = p.uuid;

    item.title = p.title.simplified();
    const QString subtitle = p.subtitle.simplified();
    if (!subtitle.isEmpty() && !item.title.contains(subtitle, Qt::CaseInsensitive)) {
        // A title ending in a question already separates itself from its subtitle.
        const bool ends = !item.title.isEmpty() && QStringLiteral("?!:").contains(item.title.back());
        item.title += (ends ? QStringLiteral(" ") : QStringLiteral(": ")) + subtitle;
    }

    item.authors = p.authors;
    item.editors = p.editors;
    item.year = p.year;
    item.month = p.month;
    item.day = p.day;
    item.containerTitle = p.containerTitle.simplified();
    item.volume = p.volume.trimmed();
    item.issue = p.issue.trimmed();
    item.pages = p.pages.trimmed();
    item.edition = p.edition.trimmed();
    item.series = p.series.trimmed();
    item.publisher = p.publisher.trimmed();
    item.place = p.place.trimmed();
    item.doi = p.doi.trimmed();
    item.url = p.url.trimmed();
    item.isbn = orFrom(p.isbn13, p.isbn10);
    item.issn = orFrom(p.issn, p.eissn);
    item.arxivId = p.arxivId.trimmed();
    item.pmid = p.pmid.trimmed();
    item.conferenceName = p.conferenceName.simplified();
    item.conferenceLocation = p.conferenceLocation.simplified();
    item.institution = p.institution.simplified();
    item.department = p.department.simplified();
    item.degree = p.degree.simplified();
    item.reportNumber = p.reportNumber.trimmed();
    item.standardNumber = p.standardNumber.trimmed();
    item.standardBody = p.standardBody.simplified();
    item.court = p.court.simplified();
    item.jurisdiction = p.jurisdiction.simplified();
    item.neutralCitation = p.neutralCitation.simplified();
    item.caseNumber = p.caseNumber.simplified();
    item.oscola = p.caseCitationForm == QLatin1StringView("uk");
    item.abstract = p.abstract.trimmed();
    item.language = p.language.trimmed();
    item.keywords = keywords;

    // An article or chapter takes what it doesn't have from what it's in.
    if (holder) {
        const Publication &h = *holder;
        // An issue's title describes the issue, so only its journal's name will do. A book's title is the book's.
        if (item.containerTitle.isEmpty())
            item.containerTitle = (h.type == PublicationType::JournalIssue ? h.containerTitle : orFrom(h.containerTitle, h.title)).simplified();
        // The editors of the book or proceedings. A journal's editors aren't cited.
        if (item.editors.isEmpty() && h.type != PublicationType::JournalIssue)
            item.editors = h.editors;
        if (!item.year)
            item.year = h.year;
        if (!item.month && item.year == h.year)
            item.month = h.month;
        item.volume = orFrom(item.volume, h.volume);
        item.issue = orFrom(item.issue, h.issue);
        item.edition = orFrom(item.edition, h.edition);
        item.series = orFrom(item.series, h.series);
        item.publisher = orFrom(item.publisher, h.publisher);
        item.place = orFrom(item.place, h.place);
        if (item.isbn.isEmpty() && (item.kind == Kind::Chapter || item.kind == Kind::Paper))
            item.isbn = orFrom(h.isbn13, h.isbn10);
        item.issn = orFrom(item.issn, orFrom(h.issn, h.eissn));
        item.conferenceName = orFrom(item.conferenceName, h.conferenceName);
        item.conferenceLocation = orFrom(item.conferenceLocation, h.conferenceLocation);
    }

    // For a paper, the proceedings are known by their conference if they have no title of their own.
    if (item.kind == Kind::Paper && item.containerTitle.isEmpty())
        item.containerTitle = item.conferenceName;
    // A whole issue is known by its journal.
    if (item.kind == Kind::Issue && item.containerTitle.isEmpty())
        item.containerTitle = item.title;
    return item;
}

QList<CitationItem> citationItems(core::Database &db, const QList<qint64> &ids)
{
    core::PublicationRepository publications(db);
    core::TagRepository tags(db);
    QHash<qint64, std::optional<Publication>> holders;
    QList<CitationItem> items;
    items.reserve(ids.size());
    for (const qint64 id : ids) {
        const std::optional<Publication> publication = publications.get(id);
        if (!publication)
            continue;
        std::optional<Publication> holder;
        if (publication->parentId) {
            auto found = holders.constFind(*publication->parentId);
            if (found == holders.cend())
                found = holders.insert(*publication->parentId, publications.get(*publication->parentId));
            holder = *found;
        }
        QStringList keywords;
        for (const core::Tag &tag : tags.tagsFor(id))
            keywords << tags.path(tag.id);
        items << citationItem(*publication, holder, keywords);
    }
    return items;
}

} // namespace papertrail::citation
