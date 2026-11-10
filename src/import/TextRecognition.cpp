#include "import/TextRecognition.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/FullTextIndex.h"
#include "import/ImportService.h"
#include "import/WorkerClient.h"

#include <QDateTime>
#include <QFileInfo>

namespace papertrail::import {

using namespace core;

namespace {

// Files with no text of their own, of the kinds with fixed pages to recognise, in the library, and not yet done with
// these languages.
const char *const kWaiting =
    " FROM file f WHERE f.sha256 IS NOT NULL AND f.sha256 <> '' AND f.status = 'ok' AND f.has_text_layer = 0 "
    "AND f.format IN ('pdf', 'djvu') "
    "AND f.root_id NOT IN (SELECT root_id FROM removed_root) "
    "AND f.id IN (SELECT file_id FROM publication_file) "
    "AND NOT EXISTS (SELECT 1 FROM fulltext.recognition r WHERE r.sha256 = f.sha256 AND r.done = 1 AND r.languages = ?1)";

} // namespace

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

    qint64 after = 0;
    while (true) {
        if (m_options.cancelled && m_options.cancelled()) {
            report.stopped = true;
            return report;
        }
        qint64 fileId = 0;
        QString sha256;
        {
            auto next = m_db.prepare(QByteArray("SELECT f.id, f.sha256") + kWaiting + " AND f.id > ?2 ORDER BY f.id LIMIT 1");
            next->bind(1, m_options.languages);
            next->bind(2, after);
            if (!next->step())
                return report;
            fileId = next->int64At(0);
            sha256 = next->textAt(1);
        }
        after = fileId;
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
        while (true) {
            if (progress)
                progress(waitingCount(), page, pageCount, name);
            const WorkerClient::Recognised result = worker.recognise(path, converted, folder, m_options.tessdata, m_options.languages,
                                                                     page, 1, again, m_options.timeoutMs);
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
            if (pageCount <= 0 || page >= pageCount)
                break;
            keep(page, pageCount, false, QString());
        }
        keep(page, pageCount, true, QString());
        ++report.documents;
        // Read into the index again, with the words recognised.
        if (recognisedHere > 0)
            index.forget(sha256);
    }
}

} // namespace papertrail::import
