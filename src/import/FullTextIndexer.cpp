#include "import/FullTextIndexer.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/FullTextIndex.h"
#include "import/ImportService.h"
#include "import/WorkerClient.h"

#include <QFileInfo>

#include <algorithm>

namespace papertrail::import {

using namespace core;

namespace {

constexpr qint64 kMinTextLetters = 40; // Fewer than this is a stray page number, not a text layer.

} // namespace

FullTextIndexer::FullTextIndexer(Database &db, const FullTextOptions &options)
    : m_db(db), m_options(options)
{
}

FullTextReport FullTextIndexer::run(const Progress &progress)
{
    FullTextReport report;
    FullTextIndex index(m_db);
    FileRepository files(m_db);
    WorkerClient worker(m_options.workerPath, m_options.cancelled);

    index.removeOrphans();

    qint64 done = 0;
    qint64 after = 0;
    while (true) {
        if (m_options.cancelled && m_options.cancelled()) {
            report.stopped = true;
            return report;
        }
        const std::optional<FullTextIndex::Pending> pending = index.nextPending(after);
        if (!pending)
            return report;
        after = pending->fileId;

        const QString path = files.absolutePath(pending->fileId);
        // Its drive may be unplugged. It'll still be waiting next time, with nothing recorded against it.
        if (!QFileInfo::exists(path))
            continue;

        const QString name = QFileInfo(path).fileName();
        if (progress)
            progress(done, index.pendingCount(), name);

        const QString converted = convertedPathFor(m_options.cacheDirectory, pending->sha256);
        const qint64 document = index.beginDocument(pending->sha256);
        int page = 0;
        int pageCount = 0;
        qint64 letters = 0;
        QString problem;
        while (true) {
            const WorkerClient::PageTexts texts = worker.pageTexts(path, converted, page, m_options.pagesPerRequest, m_options.timeoutMs);
            if (texts.failure == WorkerFailure::Cancelled) {
                report.stopped = true;
                return report; // It isn't marked complete, so it starts again next time.
            }
            if (texts.failure != WorkerFailure::None) {
                problem = texts.error.isEmpty() ? QStringLiteral("the file couldn't be read") : texts.error;
                break;
            }
            pageCount = texts.pageCount;
            index.addPages(document, page, texts.pages);
            for (const QString &text : texts.pages)
                letters += std::count_if(text.begin(), text.end(), [](QChar c) { return c.isLetter(); });
            report.pages += int(texts.pages.size());
            page += int(texts.pages.size());
            if (texts.pages.isEmpty() || page >= pageCount)
                break;
        }
        index.finishDocument(document, pageCount, problem);

        // Having read it all, this is better than the guess from the first few pages at import.
        if (problem.isEmpty() && pageCount > 0) {
            auto update = m_db.prepare("UPDATE file SET has_text_layer = ? WHERE sha256 = ?");
            update->bind(1, letters >= kMinTextLetters ? 1 : 0);
            update->bind(2, pending->sha256);
            update->step();
        }

        ++report.documents;
        if (!problem.isEmpty())
            ++report.withProblems;
        ++done;
    }
}

} // namespace papertrail::import
