#include "core/repo/VolumeArticles.h"

#include "core/db/Database.h"
#include "core/repo/PublicationRepository.h"

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

bool canHoldArticles(PublicationType type)
{
    switch (type) {
    case PublicationType::JournalVolume:
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

void markExistingArticles(Database &db, qint64 volumeId, QList<ArticleDraft> &drafts)
{
    PublicationRepository publications(db);
    QSet<QString> existing;
    for (const qint64 id : publications.childIds(volumeId)) {
        if (const std::optional<Publication> article = publications.get(id))
            existing.insert(matchKey(article->pageStart.value_or(0), article->title));
    }
    for (ArticleDraft &draft : drafts)
        draft.alreadyAdded = existing.contains(matchKey(draft.firstPage, draft.title));
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
        ids << publications.create(article);
    }
    transaction.commit();
    return ids;
}

} // namespace papertrail::core
