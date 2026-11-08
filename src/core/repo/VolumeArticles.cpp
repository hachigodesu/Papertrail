#include "core/repo/VolumeArticles.h"

#include "core/db/Database.h"
#include "core/repo/PublicationRepository.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <stdexcept>

namespace papertrail::core {

namespace {

QString tidied(const QString &title)
{
    return title.simplified();
}

QString matchKey(int firstPage, const QString &title)
{
    return QString::number(firstPage) + u'|' + tidied(title).toCaseFolded();
}

// Parts of a volume that a table of contents lists along with its articles.
bool looksLikeFrontOrBackMatter(const QString &title)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(?:(?:front|back|end)\\s*(?:matter|cover)|(?:inside\\s+)?(?:front|back)\\s+cover|cover(?:\\s+page)?|"
                       "title\\s+page|half\\s+title|copyright(?:\\s+page)?|(?:table\\s+of\\s+)?contents|masthead|"
                       "editorial\\s+board|index|author\\s+index|subject\\s+index|keyword\\s+index|blank\\s+page|"
                       "advert(?:isement)?s?)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(tidied(title)).hasMatch();
}

} // namespace

bool isPlaceholderTitle(const QString &title)
{
    static const QRegularExpression pattern(
        // Roman numerals only as they're properly written, so a word such as "Civil" isn't one.
        QStringLiteral("^(?:(?:article|paper|chapter|part|section|item|no\\.?|number|#)\\s*)?"
                       "(?:\\d+|(?=[ivxlc])(?:xc|xl|l?x{0,3})(?:ix|iv|v?i{0,3}))[.:)]?$|^untitled$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString tidy = tidied(title);
    return tidy.isEmpty() || pattern.match(tidy).hasMatch();
}

bool adoptPageHeading(ArticleDraft &draft, const QString &pageTitle, const QString &doi)
{
    bool changed = false;
    const QString heading = tidied(pageTitle);
    const auto letters = std::count_if(heading.cbegin(), heading.cend(), [](QChar c) { return c.isLetter(); });
    if (isPlaceholderTitle(draft.title) && letters >= 3 && !isPlaceholderTitle(heading)) {
        draft.title = heading;
        draft.titleFromPage = true;
        draft.likelyNotAnArticle = looksLikeFrontOrBackMatter(heading);
        changed = true;
    }
    if (draft.doi.isEmpty() && !doi.trimmed().isEmpty()) {
        draft.doi = doi.trimmed();
        changed = true;
    }
    return changed;
}

bool canHoldArticles(PublicationType type)
{
    switch (type) {
    case PublicationType::JournalIssue:
    case PublicationType::ConferenceProceedings:
    case PublicationType::Book:
    case PublicationType::Textbook:
        return true;
    default:
        return false;
    }
}

PublicationType articleTypeFor(PublicationType volumeType)
{
    switch (volumeType) {
    case PublicationType::ConferenceProceedings: return PublicationType::ConferencePaper;
    case PublicationType::Book:
    case PublicationType::Textbook: return PublicationType::BookChapter;
    default: return PublicationType::JournalArticle;
    }
}

int likelyArticleDepth(const QList<ContentsEntry> &entries)
{
    QHash<int, int> counts;
    for (const ContentsEntry &entry : entries) {
        if (entry.page >= 0)
            ++counts[entry.depth];
    }
    int best = 0;
    int bestCount = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        // A tie goes to the higher level, because a deeper one would split things up further than needed.
        if (it.value() > bestCount || (it.value() == bestCount && it.key() < best)) {
            best = it.key();
            bestCount = it.value();
        }
    }
    return best;
}

QList<ArticleDraft> draftsFromContents(const QList<ContentsEntry> &entries, int depth, int pageCount)
{
    QList<ArticleDraft> drafts;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const ContentsEntry &entry = entries.at(i);
        if (entry.depth != depth || entry.page < 0 || entry.page >= pageCount || tidied(entry.title).isEmpty())
            continue;

        // It runs up to whatever comes next at this level or above. Entries deeper down are inside it.
        // One starting on the same page shares that page, since articles often begin mid-page.
        int last = pageCount;
        for (qsizetype j = i + 1; j < entries.size(); ++j) {
            const ContentsEntry &next = entries.at(j);
            if (next.depth > depth || next.page < 0)
                continue;
            last = qMax(next.page, entry.page + 1); // Counting from 1, that's the page before the next one starts.
            break;
        }

        ArticleDraft draft;
        draft.title = tidied(entry.title);
        draft.firstPage = entry.page + 1;
        draft.lastPage = qMax(draft.firstPage, qMin(last, pageCount));
        draft.likelyNotAnArticle = looksLikeFrontOrBackMatter(draft.title);
        drafts << draft;
    }
    return drafts;
}

bool fillVolumeNumbering(Database &db, qint64 volumeId, const QString &volume, const QString &issue,
                         std::optional<int> month, std::optional<int> year)
{
    PublicationRepository publications(db);
    QList<qint64> ids = {volumeId};
    ids += publications.childIds(volumeId);
    bool changed = false;
    Transaction transaction(db);
    for (const qint64 id : std::as_const(ids)) {
        std::optional<Publication> publication = publications.get(id);
        if (!publication)
            continue;
        bool fill = false;
        if (publication->volume.isEmpty() && !volume.isEmpty()) {
            publication->volume = volume;
            fill = true;
        }
        if (publication->issue.isEmpty() && !issue.isEmpty()) {
            publication->issue = issue;
            fill = true;
        }
        // A month only means something with its own year.
        if (!publication->year && year) {
            publication->year = year;
            fill = true;
        }
        if (!publication->month && month && publication->year == year) {
            publication->month = month;
            fill = true;
        }
        if (fill) {
            publications.update(*publication);
            changed = true;
        }
    }
    transaction.commit();
    return changed;
}

void markExistingArticles(Database &db, qint64 volumeId, QList<ArticleDraft> &drafts)
{
    PublicationRepository publications(db);
    QSet<QString> existing;
    QHash<int, qint64> numberedOnly; // By first page.
    for (const qint64 id : publications.childIds(volumeId)) {
        if (const std::optional<Publication> article = publications.get(id)) {
            existing.insert(matchKey(article->pageStart.value_or(0), article->title));
            if (isPlaceholderTitle(article->title) && article->pageStart)
                numberedOnly.insert(*article->pageStart, id);
        }
    }
    for (ArticleDraft &draft : drafts) {
        draft.renames.reset();
        const auto numbered = numberedOnly.constFind(draft.firstPage);
        if (numbered != numberedOnly.cend()) {
            // It's there, but only as a number, so it's offered for naming rather than as already added.
            draft.alreadyAdded = false;
            draft.renames = numbered.value();
        } else {
            draft.alreadyAdded = existing.contains(matchKey(draft.firstPage, draft.title));
        }
    }
}

QList<qint64> createArticles(Database &db, qint64 volumeId, const QList<ArticleDraft> &drafts, bool needsReview)
{
    PublicationRepository publications(db);
    const std::optional<Publication> volume = publications.get(volumeId);
    if (!volume)
        throw std::invalid_argument("that volume doesn't exist");
    if (volume->parentId)
        throw std::invalid_argument("an article can't have articles of its own");

    Transaction transaction(db);
    QList<qint64> ids;
    for (const ArticleDraft &draft : drafts) {
        if (tidied(draft.title).isEmpty())
            throw std::invalid_argument("an article needs a title");

        if (draft.renames) {
            std::optional<Publication> existing = publications.get(*draft.renames);
            if (!existing || existing->parentId != volumeId)
                throw std::invalid_argument("the article to be named isn't part of this volume");
            // Only a number is replaced, so a title someone has typed since is never lost.
            bool changed = false;
            if (isPlaceholderTitle(existing->title) && tidied(draft.title) != tidied(existing->title)) {
                existing->title = tidied(draft.title);
                changed = true;
            }
            if (existing->doi.isEmpty() && !draft.doi.isEmpty()) {
                existing->doi = draft.doi;
                changed = true;
            }
            if (changed) {
                existing->needsReview = existing->needsReview || needsReview;
                publications.update(*existing);
            }
            ids << existing->id;
            continue;
        }

        Publication article;
        article.type = articleTypeFor(volume->type);
        article.parentId = volumeId;
        article.pageStart = draft.firstPage;
        article.pageEnd = draft.lastPage;
        article.title = tidied(draft.title);
        article.authors = draft.authors;
        article.doi = draft.doi;
        article.needsReview = needsReview;

        // What it shares with the volume. A journal volume's own title is often the journal's name.
        article.containerTitle = volume->containerTitle.isEmpty() ? volume->title : volume->containerTitle;
        article.volume = volume->volume;
        article.issue = volume->issue;
        article.year = volume->year;
        article.month = volume->month;
        article.publisher = volume->publisher;
        article.place = volume->place;
        article.language = volume->language;
        article.issn = volume->issn;
        article.eissn = volume->eissn;
        article.conferenceName = volume->conferenceName;
        article.conferenceLocation = volume->conferenceLocation;
        const qint64 id = publications.create(article);
        auto join = db.prepare("INSERT OR IGNORE INTO collection_publication (collection_id, publication_id, added_at) "
                               "SELECT collection_id, ?, ? FROM collection_publication WHERE publication_id = ?");
        join->bind(1, id);
        join->bind(2, QDateTime::currentMSecsSinceEpoch());
        join->bind(3, volumeId);
        join->step();
        ids << id;
    }
    transaction.commit();
    return ids;
}

} // namespace papertrail::core
