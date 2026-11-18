#include "core/query/LibraryMentions.h"

#include "core/db/Database.h"
#include "core/query/LibraryQuery.h"
#include "core/query/SearchParser.h"
#include "core/repo/AnnotationRepository.h"
#include "core/repo/FullTextIndex.h"

#include <algorithm>

namespace papertrail::core {

namespace {

// The words as one phrase for the search box's parser, with nothing in them that it would read as syntax.
QString phraseFor(const QString &words)
{
    QString phrase = words;
    phrase.replace(u'"', u' ');
    phrase = phrase.simplified();
    return phrase.isEmpty() ? QString() : u'"' + phrase + u'"';
}

} // namespace

Mentions mentionsOf(Database &db, const QString &words, qint64 thisPublication, int maxPublications,
                    int publicationsWithPages, int maxHighlights)
{
    Mentions mentions;
    const QString phrase = phraseFor(words);
    if (phrase.isEmpty())
        return mentions;

    const bool withPages = hasFullText(db);
    PublicationFilter filter;
    filter.searchText = phrase;
    filter.searchContents = withPages;
    LibraryQuery query(db);
    mentions.publications = query.count(filter);
    const QList<PublicationSummary> publications = query.page(filter, {SortField::Relevance, true}, 0, maxPublications);

    const ParsedSearch parsed = parseSearch(phrase);
    FullTextIndex index(db);
    int withPagesLooked = 0;
    QList<Mention> pages;
    QList<Mention> details;
    for (const PublicationSummary &publication : publications) {
        Mention base;
        base.publicationId = publication.id;
        base.title = publication.title;
        base.authors = publication.authors;
        base.year = publication.year;
        base.thisDocument = publication.id == thisPublication;
        QList<ContentMatch> matches;
        if (withPages && !parsed.contentsMatch.isEmpty()
            && (base.thisDocument || withPagesLooked < publicationsWithPages)) {
            if (!base.thisDocument)
                ++withPagesLooked;
            matches = index.matchesFor(publication.id, parsed.contentsMatch, base.thisDocument ? 10 : 3);
        }
        if (matches.isEmpty()) {
            details << base;
            continue;
        }
        for (const ContentMatch &match : matches) {
            Mention page = base;
            page.kind = Mention::Kind::Page;
            page.fileId = match.fileId;
            page.page = match.page;
            page.snippet = match.snippet.simplified();
            pages << page;
        }
    }

    QList<Mention> highlights;
    AnnotationFilter annotations;
    annotations.search = words.simplified();
    annotations.limit = maxHighlights;
    if (!annotations.search.isEmpty()) {
        for (const AnnotationListing &listing : AnnotationRepository(db).everywhere(annotations)) {
            Mention highlight;
            highlight.kind = Mention::Kind::Highlight;
            highlight.publicationId = listing.annotation.publicationId;
            highlight.title = listing.publicationTitle;
            highlight.thisDocument = listing.annotation.publicationId == thisPublication;
            highlight.annotationId = listing.annotation.id;
            highlight.page = listing.annotation.firstPage();
            QString text = listing.annotation.selectedText().simplified();
            const QString note = listing.annotation.note.simplified();
            if (!note.isEmpty())
                text = text.isEmpty() ? note : text + QStringLiteral(" — ") + note;
            highlight.snippet = text.size() > 300 ? text.left(300) + QStringLiteral("...") : text;
            highlights << highlight;
        }
    }

    // What's in the document being read comes first, then pages, highlights, and the rest.
    auto thisFirst = [](const Mention &a, const Mention &b) { return a.thisDocument && !b.thisDocument; };
    std::stable_sort(pages.begin(), pages.end(), thisFirst);
    std::stable_sort(highlights.begin(), highlights.end(), thisFirst);
    std::stable_sort(details.begin(), details.end(), thisFirst);
    mentions.found = pages + highlights + details;
    return mentions;
}

} // namespace papertrail::core
