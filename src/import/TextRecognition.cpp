#include "import/TextRecognition.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/FullTextIndex.h"
#include "import/ImportService.h"
#include "import/WorkerClient.h"

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>

namespace papertrail::import {

using namespace core;

namespace {

// Files with no text of their own, or with some pages that have little or none, or asked for in full, of the kinds
// with fixed pages to recognise, in the library, and not yet done with these languages.
const char *const kWaiting =
    " FROM file f WHERE f.sha256 IS NOT NULL AND f.sha256 <> '' AND f.status = 'ok' "
    "AND (f.has_text_layer = 0 OR f.sha256 IN (SELECT sha256 FROM fulltext.recognition_wanted) "
    "OR (f.has_text_layer = 1 AND f.sha256 IN (SELECT sha256 FROM fulltext.mixed_pages WHERE pages <> ''))) "
    "AND f.format IN ('pdf', 'djvu') "
    "AND f.root_id NOT IN (SELECT root_id FROM removed_root) "
    "AND f.id IN (SELECT file_id FROM publication_file) "
    "AND NOT EXISTS (SELECT 1 FROM fulltext.recognition r WHERE r.sha256 = f.sha256 AND r.done = 1 AND r.languages = ?1)";

// The order they're done in: those asked for, then wholly scanned documents, then the scanned pages of others.
const char *const kPasses[] = {
    " AND f.sha256 IN (SELECT sha256 FROM fulltext.recognition_wanted)",
    " AND f.has_text_layer = 0",
    "",
};

// Fewer of a page's own letters than this, and it may be a scan. The worker's looksScanned decides.
constexpr int kFewestLetters = 20;

} // namespace

void TextRecognition::noteMixedPages()
{
    // Documents with text of their own, read into the index in full, whose pages haven't been looked at yet. Before
    // anything's recognised, a page's text in the index is the file's own, and a page with none isn't in it at all.
    auto next = m_db.prepare(
        "SELECT d.id, d.sha256, d.page_count FROM fulltext.document d WHERE d.complete = 1 AND d.problem = '' "
        "AND d.id > ? AND d.sha256 IN (SELECT sha256 FROM file WHERE has_text_layer = 1 AND format IN ('pdf', 'djvu')) "
        "AND NOT EXISTS (SELECT 1 FROM fulltext.mixed_pages m WHERE m.sha256 = d.sha256) ORDER BY d.id LIMIT 1");
    Statement &pages = m_db.cached("SELECT rowid, text FROM fulltext.page_text WHERE rowid >= ? AND rowid < ?");
    Statement &note = m_db.cached("INSERT OR REPLACE INTO fulltext.mixed_pages (sha256, pages) VALUES (?, ?)");
    qint64 after = 0;
    while (!(m_options.cancelled && m_options.cancelled())) {
        next->reset();
        next->bind(1, after);
        if (!next->step())
            return;
        const qint64 document = next->int64At(0);
        const QString sha256 = next->textAt(1);
        const int pageCount = next->intAt(2);
        after = document;

        QList<bool> enough(pageCount, false);
        pages.bind(1, document * kFullTextPagesPerDocument);
        pages.bind(2, (document + 1) * kFullTextPagesPerDocument);
        while (pages.step()) {
            const qint64 page = pages.int64At(0) - document * kFullTextPagesPerDocument;
            const QString text = pages.textAt(1);
            if (page >= 0 && page < pageCount)
                enough[page] = std::count_if(text.cbegin(), text.cend(), [](QChar c) { return c.isLetter(); }) >= kFewestLetters;
        }
        pages.reset();
        QStringList thin;
        for (int page = 0; page < pageCount; ++page) {
            if (!enough.at(page))
                thin << QString::number(page);
        }
        note.bind(1, sha256);
        note.bind(2, thin.isEmpty() ? QStringLiteral("") : thin.join(u','));
        note.step();
        note.reset();
    }
}

void TextRecognition::want(Database &db, const QString &sha256)
{
    Transaction transaction(db);
    auto insert = db.prepare("INSERT INTO fulltext.recognition_wanted (sha256, wanted_at) VALUES (?, ?) "
                             "ON CONFLICT (sha256) DO UPDATE SET wanted_at = excluded.wanted_at");
    insert->bind(1, sha256);
    insert->bind(2, QDateTime::currentMSecsSinceEpoch());
    insert->step();
    // Pages already recognised are kept, and the rest are done.
    auto restart = db.prepare("DELETE FROM fulltext.recognition WHERE sha256 = ?");
    restart->bind(1, sha256);
    restart->step();
    transaction.commit();
}

bool TextRecognition::isWanted(Database &db, const QString &sha256)
{
    auto select = db.prepare("SELECT 1 FROM fulltext.recognition_wanted WHERE sha256 = ?");
    select->bind(1, sha256);
    return select->step();
}

TextRecognition::TextRecognition(Database &db, const RecognitionOptions &options)
    : m_db(db), m_options(options)
{
}

qint64 TextRecognition::waitingCount()
{
    auto select = m_db.prepare(QByteArray("SELECT count(DISTINCT f.sha256)") + kWaiting);
    select->bind(1, m_options.languages);
    select->step();
    return select->int64At(0);
}

RecognitionReport TextRecognition::run(const Progress &progress)
{
    RecognitionReport report;
    FileRepository files(m_db);
    FullTextIndex index(m_db);
    WorkerClient worker(m_options.workerPath, m_options.cancelled);
    worker.setLowPriority(true);
    noteMixedPages();

    qint64 after = 0;
    size_t pass = 0;
    while (true) {
        if (m_options.cancelled && m_options.cancelled()) {
            report.stopped = true;
            return report;
        }
        qint64 fileId = 0;
        QString sha256;
        bool ownText = false;
        {
            auto next = m_db.prepare(QByteArray("SELECT f.id, f.sha256, f.has_text_layer") + kWaiting + kPasses[pass]
                                     + " AND f.id > ?2 ORDER BY f.id LIMIT 1");
            next->bind(1, m_options.languages);
            next->bind(2, after);
            if (!next->step()) {
                if (++pass >= std::size(kPasses))
                    return report;
                after = 0;
                continue;
            }
            fileId = next->int64At(0);
            sha256 = next->textAt(1);
            ownText = next->intAt(2) == 1;
        }
        after = fileId;
        const bool everyPage = isWanted(m_db, sha256);
        // A document with text of its own has only its pages with little or none looked at.
        QList<int> onlyPages;
        if (ownText && !everyPage) {
            auto noted = m_db.prepare("SELECT pages FROM fulltext.mixed_pages WHERE sha256 = ?");
            noted->bind(1, sha256);
            if (noted->step()) {
                for (const QString &page : noted->textAt(0).split(u',', Qt::SkipEmptyParts))
                    onlyPages << page.toInt();
            }
            if (onlyPages.isEmpty())
                continue;
        }
        const QString path = files.absolutePath(fileId);
        if (!QFileInfo::exists(path))
            continue; // Its drive may be unplugged. It's still waiting next time.

        // Where it got to, if it's been started with these languages.
        int page = 0;
        int recognised = 0;
        bool again = false; // Recognised before with other languages, so its pages are read again.
        {
            auto state = m_db.prepare("SELECT next_page, recognised, languages FROM fulltext.recognition WHERE sha256 = ?");
            state->bind(1, sha256);
            if (state->step()) {
                if (state->textAt(2) == m_options.languages) {
                    page = state->intAt(0);
                    recognised = state->intAt(1);
                } else {
                    again = true;
                }
            }
        }
        const auto keep = [&](int nextPage, int pageCount, bool done, const QString &problem) {
            auto upsert = m_db.prepare(
                "INSERT INTO fulltext.recognition (sha256, languages, next_page, page_count, recognised, done, problem, updated_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (sha256) DO UPDATE SET languages = excluded.languages, "
                "next_page = excluded.next_page, page_count = excluded.page_count, recognised = excluded.recognised, "
                "done = excluded.done, problem = excluded.problem, updated_at = excluded.updated_at");
            upsert->bind(1, sha256);
            upsert->bind(2, m_options.languages);
            upsert->bind(3, nextPage);
            upsert->bind(4, pageCount);
            upsert->bind(5, recognised);
            upsert->bind(6, done ? 1 : 0);
            upsert->bind(7, problem.isNull() ? QStringLiteral("") : problem);
            upsert->bind(8, QDateTime::currentMSecsSinceEpoch());
            upsert->step();
        };

        const QString folder = recognisedFolderFor(m_options.cacheDirectory, sha256);
        const QString converted = convertedPathFor(m_options.cacheDirectory, sha256);
        const QString name = QFileInfo(path).fileName();
        int pageCount = 0;
        int recognisedHere = 0;
        // The next of the pages to look at, from where it got to.
        const auto nextListed = [&onlyPages](int from) {
            const auto found = std::lower_bound(onlyPages.cbegin(), onlyPages.cend(), from);
            return found == onlyPages.cend() ? -1 : *found;
        };
        if (!onlyPages.isEmpty()) {
            pageCount = onlyPages.last() + 1;
            page = nextListed(page);
        }
        while (page >= 0) {
            if (progress)
                progress(waitingCount(), page, pageCount, name);
            const WorkerClient::Recognised result = worker.recognise(path, converted, folder, m_options.tessdata, m_options.languages,
                                                                     page, 1, again, m_options.timeoutMs, everyPage);
            if (result.failure == WorkerFailure::Cancelled) {
                keep(page, pageCount, false, QString());
                report.stopped = true;
                return report;
            }
            if (result.failure == WorkerFailure::Rejected) {
                // Such as a language that isn't there, which every document would meet too.
                keep(page, pageCount, false, result.error);
                report.problem = result.error;
                return report;
            }
            // A page that crashed or hung the worker is passed over, and the rest are still read.
            if (result.failure == WorkerFailure::None) {
                pageCount = result.pageCount;
                recognised += result.pages;
                recognisedHere += result.pages;
                report.pages += result.pages;
            }
            ++page;
            if (!onlyPages.isEmpty())
                page = nextListed(page);
            if (page < 0 || pageCount <= 0 || page >= pageCount)
                break;
            keep(page, pageCount, false, QString());
        }
        keep(qMax(page, 0), pageCount, true, QString());
        ++report.documents;
        // Read into the index again, with the words recognised.
        if (recognisedHere > 0)
            index.forget(sha256);
    }
}

} // namespace papertrail::import
