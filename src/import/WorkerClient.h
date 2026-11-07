#pragma once

#include "import/Extractors.h"

#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace papertrail::import {

// What the worker found in a file.
struct InspectResult {
    QString format;
    bool isKf8 = false;      // Was read through a converted copy.
    bool encrypted = false;  // Needs a password.
    bool blank = false;      // Opened, but gave no visible content or text.
    bool coverWritten = false;
    std::optional<int> pageCount;
    std::optional<bool> hasTextLayer;
    QString problem; // Why it couldn't be read, when there's something to say beyond failing.
    QString largestText;
    QString text; // Text from the first pages, each ending in a form feed.
    EmbeddedMetadata metadata;
};

enum class WorkerFailure {
    None,
    Crashed,  // The worker died, most likely because of this file.
    TimedOut, // The worker took too long, and has been stopped.
    Rejected, // The worker reported that it couldn't read the file.
    Cancelled, // Stopped on request. Says nothing about the file.
};

// Runs papertrail-worker and sends it one request at a time. It's restarted whenever it's needed
// again after a crash or timeout, so a bad file can't take the app down with it.
class WorkerClient {
public:
    // `cancelled`, if given, is checked while waiting, and stops the worker when it returns true.
    explicit WorkerClient(const QString &workerPath, std::function<bool()> cancelled = {});
    ~WorkerClient();

    struct HashResult {
        WorkerFailure failure = WorkerFailure::None;
        QString error;
        QString sha256;
    };
    HashResult hash(const QString &path, int timeoutMs);

    struct Inspection {
        WorkerFailure failure = WorkerFailure::None;
        QString error;
        InspectResult result;
    };
    Inspection inspect(const QString &path, const QString &coverPath, const QString &convertedPath, int timeoutMs);

    struct PageTexts {
        WorkerFailure failure = WorkerFailure::None;
        QString error;
        int pageCount = 0;
        QStringList pages;
    };
    PageTexts pageTexts(const QString &path, const QString &convertedPath, int start, int count, int timeoutMs);

    // How many times the worker has been started, for tests and reports.
    int startCount() const { return m_startCount; }

private:
    struct Reply {
        WorkerFailure failure = WorkerFailure::None;
        QString error;
        QJsonObject body;
    };
    Reply call(QJsonObject request, int timeoutMs);
    bool ensureRunning();
    void stop();

    QString m_workerPath;
    std::function<bool()> m_cancelled;
    std::unique_ptr<QProcess> m_process;
    qint64 m_nextId = 1;
    int m_startCount = 0;
};

} // namespace papertrail::import
