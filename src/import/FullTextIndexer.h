#pragma once

#include <QString>

#include <functional>

namespace papertrail::core {
class Database;
}

namespace papertrail::import {

struct FullTextOptions {
    QString workerPath;
    QString cacheDirectory; // Where a converted Kindle book's readable copy is kept.
    int pagesPerRequest = 25;
    int timeoutMs = 120000; // For each request, which is what a bad page can hold up.

    // Checked while waiting for the worker, so that stopping doesn't have to wait for a large file.
    std::function<bool()> cancelled;
};

struct FullTextReport {
    int documents = 0;
    int pages = 0;
    int withProblems = 0; // What could be read of these is still searchable.
    bool stopped = false;
};

// Reads the text of whatever hasn't been indexed into the full-text index, through the worker. There's no
// list of work to keep: what's left is always whatever files the index doesn't have yet, so stopping is safe
// and new files are picked up by themselves. The database connection needs the index attached.
class FullTextIndexer {
public:
    FullTextIndexer(core::Database &db, const FullTextOptions &options);

    // Called after each document, and while reading a long one.
    using Progress = std::function<void(qint64 done, qint64 remaining, const QString &fileName)>;
    FullTextReport run(const Progress &progress = {});

private:
    core::Database &m_db;
    FullTextOptions m_options;
};

} // namespace papertrail::import
