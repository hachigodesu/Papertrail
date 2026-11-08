#include "import/JournalIssues.h"

#include "core/db/Database.h"
#include "core/repo/PublicationRepository.h"
#include "import/Extractors.h"

#include "core/model/Identifiers.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace papertrail::import {

using core::Publication;
using core::PublicationRepository;
using core::PublicationType;

namespace {

constexpr const char *kSource = "issue details";

QString englishMonth(int month)
{
    static const QStringList months = {QStringLiteral("January"), QStringLiteral("February"), QStringLiteral("March"),
                                       QStringLiteral("April"),   QStringLiteral("May"),      QStringLiteral("June"),
                                       QStringLiteral("July"),    QStringLiteral("August"),   QStringLiteral("September"),
                                       QStringLiteral("October"), QStringLiteral("November"), QStringLiteral("December")};
    return month >= 1 && month <= 12 ? months.at(month - 1) : QString();
}

QSet<QString> fieldsSetByHand(core::Database &db, qint64 id)
{
    QSet<QString> fields;
    auto select = db.prepare("SELECT DISTINCT field FROM metadata_provenance WHERE publication_id = ? AND source = 'manual'");
    select->bind(1, id);
    while (select->step())
        fields.insert(select->textAt(0));
    return fields;
}

} // namespace

bool isDescriptiveIssueTitle(const QString &title, const QString &journal)
{
    const QString text = title.simplified();
    if (text.isEmpty())
        return true;
    if (!journal.trimmed().isEmpty() && text.compare(journal.simplified(), Qt::CaseInsensitive) == 0)
        return true;
    if (namesAnIssuePage(text))
        return true;
    // Numbers and a date are a cover's, not a title.
    return findIssueNumbering(text).has_value();
}

bool namesAnIssuePage(const QString &text)
{
    static const QRegularExpression pages(
        QStringLiteral("\\b(publication information|information for authors|instructions for authors|table of contents|"
                       "front cover|back cover|cover page|masthead|editorial board|issn)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return pages.match(text.simplified()).hasMatch();
}

QList<core::ArticleDraft> draftsFromPages(QList<PageHeading> pages, int pageCount)
{
    std::sort(pages.begin(), pages.end(), [](const PageHeading &a, const PageHeading &b) { return a.page < b.page; });
    // The publisher's part of a DOI, before its slash, most of the issue's DOIs share.
    QHash<QString, int> registrants;
    for (const PageHeading &page : std::as_const(pages)) {
        for (const QString &doi : page.dois)
            ++registrants[core::normaliseDoi(doi).section(u'/', 0, 0)];
    }
    QString publisher;
    for (const PageHeading &page : std::as_const(pages)) {
        for (const QString &doi : page.dois) {
            const QString registrant = core::normaliseDoi(doi).section(u'/', 0, 0);
            if (!registrant.isEmpty() && (publisher.isEmpty() || registrants.value(registrant) > registrants.value(publisher)))
                publisher = registrant;
        }
    }
    QList<core::ArticleDraft> drafts;
    QString current; // The DOI of the article whose pages these are, in lower case.
    bool open = false;
    const auto close = [&](int firstPageNotIn) {
        if (open)
            drafts.last().lastPage = qMax(drafts.last().firstPage, firstPageNotIn);
        open = false;
    };
    for (const PageHeading &page : std::as_const(pages)) {
        if (page.page < 0 || page.page >= pageCount)
            continue;
        // The first of its publisher's DOIs that isn't the article's already under way.
        QString doi;
        for (const QString &candidate : page.dois) {
            const QString normal = core::normaliseDoi(candidate).toLower();
            if (!normal.isEmpty() && normal.section(u'/', 0, 0) == publisher && normal != current) {
                doi = normal;
                break;
            }
        }
        // A page that only repeats the article's own DOI, such as in its footer, is still that article's.
        const bool anyDoi = std::any_of(page.dois.cbegin(), page.dois.cend(),
                                        [](const QString &candidate) { return !core::normaliseDoi(candidate).isEmpty(); });
        const QString title = page.title.simplified();
        if (!doi.isEmpty() && doi != current) {
            close(page.page);
            core::ArticleDraft draft;
            draft.title = title;
            draft.doi = doi;
            draft.firstPage = page.page + 1;
            draft.lastPage = page.page + 1;
            draft.titleFromPage = true;
            // Offered, but not chosen, if it looks like one of the issue's own pages.
            draft.likelyNotAnArticle = title.isEmpty() || namesAnIssuePage(title);
            drafts << draft;
            current = doi;
            open = true;
        } else if (open && !anyDoi && !title.isEmpty() && namesAnIssuePage(title)) {
            // The issue's own pages at the end aren't part of its last article.
            close(page.page);
            current.clear();
        }
    }
    close(pageCount);
    return drafts;
}

QString issueTitle(const Publication &issue)
{
    const QString journal = issue.containerTitle.simplified();
    if (journal.isEmpty())
        return {};
    QStringList parts = {journal};
    if (!issue.volume.trimmed().isEmpty())
        parts << QStringLiteral("Vol. ") + issue.volume.trimmed();
    if (!issue.issue.trimmed().isEmpty())
        parts << QStringLiteral("No. ") + issue.issue.trimmed();
    QString title = parts.join(QStringLiteral(", "));
    if (issue.year) {
        const QString when = issue.month ? englishMonth(*issue.month) + u' ' + QString::number(*issue.year) : QString::number(*issue.year);
        title += QStringLiteral(" (") + when + u')';
    }
    return title;
}

bool tidyJournalIssue(core::Database &db, qint64 issueId, const std::optional<Publication> &article)
{
    PublicationRepository publications(db);
    const std::optional<Publication> before = publications.get(issueId);
    if (!before || before->type != PublicationType::JournalIssue || before->parentId)
        return false;
    const QSet<QString> byHand = fieldsSetByHand(db, issueId);
    const auto free = [&](const char *field) { return !byHand.contains(QString::fromLatin1(field)); };

    Publication issue = *before;
    // Numbers printed in the title, as on a cover.
    if (const std::optional<IssueNumbering> numbering = findIssueNumbering(issue.title)) {
        if (issue.volume.trimmed().isEmpty() && free("volume"))
            issue.volume = numbering->volume;
        if (issue.issue.trimmed().isEmpty() && free("issue"))
            issue.issue = numbering->issue;
        if (!issue.year && numbering->year && free("year"))
            issue.year = numbering->year;
        if (!issue.month && numbering->month && issue.year == numbering->year && free("month"))
            issue.month = numbering->month;
    }
    if (issue.issn.trimmed().isEmpty() && issue.eissn.trimmed().isEmpty() && free("issn")) {
        const QString issn = findIssn(issue.title);
        if (!issn.isEmpty())
            issue.issn = issn;
    }
    // The journal, from an article that knows it.
    if (article) {
        if (issue.containerTitle.trimmed().isEmpty() && free("container_title")
            && !isDescriptiveIssueTitle(article->containerTitle, QString()))
            issue.containerTitle = article->containerTitle.simplified();
        if (issue.issn.trimmed().isEmpty() && free("issn"))
            issue.issn = article->issn.trimmed();
        if (issue.publisher.trimmed().isEmpty() && free("publisher"))
            issue.publisher = article->publisher.trimmed();
    }
    // With no journal known, a title that isn't a cover's words is most likely the journal's name, since an issue's
    // title comes from the largest text on its cover.
    if (issue.containerTitle.trimmed().isEmpty() && free("container_title") && !isDescriptiveIssueTitle(issue.title, QString()))
        issue.containerTitle = issue.title.simplified();
    // A title that only describes the issue becomes one made from its details.
    const QString oldTitle = issue.title;
    if (free("title") && isDescriptiveIssueTitle(issue.title, issue.containerTitle)) {
        const QString made = issueTitle(issue);
        if (!made.isEmpty())
            issue.title = made;
    }

    // Articles that took a cover's words as their journal's name, with nothing better to go on yet.
    QList<qint64> misnamed;
    if (issue.containerTitle.trimmed().isEmpty() && isDescriptiveIssueTitle(oldTitle, QString())) {
        for (const qint64 id : publications.childIds(issueId)) {
            const std::optional<Publication> child = publications.get(id);
            if (child && !child->containerTitle.isEmpty() && child->containerTitle.simplified() == oldTitle.simplified()
                && !fieldsSetByHand(db, id).contains(QStringLiteral("container_title")))
                misnamed << id;
        }
    }

    const bool changed = !misnamed.isEmpty() || issue.title != before->title || issue.volume != before->volume || issue.issue != before->issue
        || issue.year != before->year || issue.month != before->month || issue.issn != before->issn
        || issue.containerTitle != before->containerTitle || issue.publisher != before->publisher;
    if (!changed)
        return false;

    core::Transaction transaction(db);
    const qint64 when = QDateTime::currentMSecsSinceEpoch();
    publications.updateWithProvenance(issue, QString::fromLatin1(kSource), when);
    for (const qint64 id : std::as_const(misnamed)) {
        Publication child = *publications.get(id);
        child.containerTitle.clear(); // So it shows as missing, and a lookup can fill it in.
        publications.updateWithProvenance(child, QString::fromLatin1(kSource), when);
    }
    // Articles made when only the old title was known took it as their journal's name.
    if (!issue.containerTitle.isEmpty()) {
        for (const qint64 id : publications.childIds(issueId)) {
            std::optional<Publication> child = publications.get(id);
            if (!child || fieldsSetByHand(db, id).contains(QStringLiteral("container_title")))
                continue;
            const QString current = child->containerTitle.simplified();
            if (current.isEmpty() || current == oldTitle.simplified() || isDescriptiveIssueTitle(current, QString())) {
                child->containerTitle = issue.containerTitle;
                if (child->volume.trimmed().isEmpty())
                    child->volume = issue.volume;
                if (child->issue.trimmed().isEmpty())
                    child->issue = issue.issue;
                if (!child->year) {
                    child->year = issue.year;
                    child->month = issue.month;
                }
                publications.updateWithProvenance(*child, QString::fromLatin1(kSource), when);
            }
        }
    }
    transaction.commit();
    return true;
}

int tidyJournalIssues(core::Database &db)
{
    QList<qint64> ids;
    auto select = db.prepare(QByteArray("SELECT id FROM publication WHERE parent_id IS NULL AND type = '")
                             + core::toDbString(PublicationType::JournalIssue).toLatin1() + "'");
    while (select->step())
        ids << select->int64At(0);
    select.reset();
    int changed = 0;
    for (const qint64 id : ids) {
        // The journal can come from an article that knows it, such as one that's been looked up.
        std::optional<Publication> known;
        PublicationRepository publications(db);
        for (const qint64 child : publications.childIds(id)) {
            const std::optional<Publication> article = publications.get(child);
            if (article && !article->containerTitle.trimmed().isEmpty() && !isDescriptiveIssueTitle(article->containerTitle, QString())) {
                known = article;
                break;
            }
        }
        changed += tidyJournalIssue(db, id, known) ? 1 : 0;
    }
    return changed;
}

} // namespace papertrail::import
