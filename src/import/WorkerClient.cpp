#include "import/WorkerClient.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>

namespace papertrail::import {

WorkerClient::WorkerClient(const QString &workerPath, std::function<bool()> cancelled)
    : m_workerPath(workerPath), m_cancelled(std::move(cancelled))
{
}

WorkerClient::~WorkerClient()
{
    stop();
}

bool WorkerClient::ensureRunning()
{
    if (m_process && m_process->state() == QProcess::Running)
        return true;

    m_process = std::make_unique<QProcess>();
    m_process->setProgram(m_workerPath);
    m_process->setStandardErrorFile(QProcess::nullDevice()); // Libraries' messages aren't needed.
    if (m_lowPriority) {
#ifdef Q_OS_WIN
        m_process->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *arguments) { arguments->flags |= BELOW_NORMAL_PRIORITY_CLASS; });
#else
        m_process->setChildProcessModifier([] { [[maybe_unused]] const int ignored = nice(10); });
#endif
    }
    m_process->start(QIODevice::ReadWrite);
    ++m_startCount;
    return m_process->waitForStarted(30000);
}

void WorkerClient::stop()
{
    if (!m_process)
        return;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(5000);
    }
    m_process.reset();
}

WorkerClient::Reply WorkerClient::call(QJsonObject request, int timeoutMs)
{
    if (!ensureRunning()) {
        const QString error = m_process ? m_process->errorString() : QString();
        stop();
        return {WorkerFailure::Crashed, QStringLiteral("could not start the worker: ") + error, {}};
    }

    const qint64 id = m_nextId++;
    request.insert(QStringLiteral("id"), id);
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');

    const QDeadlineTimer deadline(timeoutMs);
    while (true) {
        while (m_process->canReadLine()) {
            const QJsonDocument document = QJsonDocument::fromJson(m_process->readLine());
            // A library could print something of its own, so anything else is skipped.
            if (document.isObject() && document.object().value(QStringLiteral("id")).toInteger() == id) {
                const QJsonObject body = document.object();
                if (!body.value(QStringLiteral("ok")).toBool()) {
                    return {WorkerFailure::Rejected, body.value(QStringLiteral("error")).toString(), body};
                }
                return {WorkerFailure::None, QString(), body};
            }
        }

        if (m_process->state() == QProcess::NotRunning) {
            stop();
            return {WorkerFailure::Crashed, QStringLiteral("the worker stopped unexpectedly"), {}};
        }
        if (m_cancelled && m_cancelled()) {
            stop();
            return {WorkerFailure::Cancelled, QStringLiteral("stopped on request"), {}};
        }
        if (deadline.hasExpired()) {
            stop();
            return {WorkerFailure::TimedOut, QStringLiteral("the worker took too long"), {}};
        }
        // Wakes for output or for the process ending, with a short wait so neither gets missed.
        m_process->waitForReadyRead(qMin<qint64>(200, qMax<qint64>(1, deadline.remainingTime())));
    }
}

WorkerClient::HashResult WorkerClient::hash(const QString &path, int timeoutMs)
{
    QJsonObject request;
    request.insert(QStringLiteral("op"), QStringLiteral("hash"));
    request.insert(QStringLiteral("path"), path);
    const Reply reply = call(request, timeoutMs);
    return {reply.failure, reply.error, reply.body.value(QStringLiteral("sha256")).toString()};
}

WorkerClient::PageTexts WorkerClient::pageTexts(const QString &path, const QString &convertedPath, int start, int count, int timeoutMs,
                                                const QString &ocrFolder)
{
    QJsonObject request;
    request.insert(QStringLiteral("op"), QStringLiteral("pagetext"));
    request.insert(QStringLiteral("path"), path);
    request.insert(QStringLiteral("convertedPath"), convertedPath);
    if (!ocrFolder.isEmpty())
        request.insert(QStringLiteral("ocrFolder"), ocrFolder);
    request.insert(QStringLiteral("start"), start);
    request.insert(QStringLiteral("count"), count);
    const Reply reply = call(request, timeoutMs);

    PageTexts texts;
    texts.failure = reply.failure;
    texts.error = reply.error;
    if (reply.failure != WorkerFailure::None)
        return texts;
    texts.pageCount = reply.body.value(QStringLiteral("pageCount")).toInt();
    const QJsonArray pages = reply.body.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &page : pages)
        texts.pages << page.toString();
    for (const QJsonValue &page : reply.body.value(QStringLiteral("recognised")).toArray())
        texts.recognised << page.toInt();
    return texts;
}

WorkerClient::Recognised WorkerClient::recognise(const QString &path, const QString &convertedPath, const QString &ocrFolder,
                                                 const QString &tessdata, const QString &languages, int start, int count,
                                                 bool again, int timeoutMs)
{
    QJsonObject request;
    request.insert(QStringLiteral("op"), QStringLiteral("ocr"));
    request.insert(QStringLiteral("path"), path);
    request.insert(QStringLiteral("convertedPath"), convertedPath);
    request.insert(QStringLiteral("ocrFolder"), ocrFolder);
    request.insert(QStringLiteral("tessdata"), tessdata);
    request.insert(QStringLiteral("languages"), languages);
    request.insert(QStringLiteral("start"), start);
    request.insert(QStringLiteral("count"), count);
    request.insert(QStringLiteral("again"), again);
    const Reply reply = call(request, timeoutMs);

    Recognised result;
    result.failure = reply.failure;
    result.error = reply.error;
    if (reply.failure != WorkerFailure::None)
        return result;
    result.pageCount = reply.body.value(QStringLiteral("pageCount")).toInt();
    result.pages = reply.body.value(QStringLiteral("recognised")).toInt();
    result.words = reply.body.value(QStringLiteral("words")).toInt();
    return result;
}

WorkerClient::Cover WorkerClient::pageCover(const QString &path, const QString &convertedPath, int page,
                                            const QString &coverPath, int timeoutMs)
{
    QJsonObject request;
    request.insert(QStringLiteral("op"), QStringLiteral("cover"));
    request.insert(QStringLiteral("path"), path);
    request.insert(QStringLiteral("convertedPath"), convertedPath);
    request.insert(QStringLiteral("page"), page);
    request.insert(QStringLiteral("coverPath"), coverPath);
    const Reply reply = call(request, timeoutMs);
    return Cover{reply.failure, reply.error};
}

WorkerClient::MarkedCopy WorkerClient::markedCopy(const QString &path, const QString &out, const QJsonArray &marks, int timeoutMs)
{
    QJsonObject request;
    request.insert(QStringLiteral("op"), QStringLiteral("markedcopy"));
    request.insert(QStringLiteral("path"), path);
    request.insert(QStringLiteral("out"), out);
    request.insert(QStringLiteral("marks"), marks);
    const Reply reply = call(request, timeoutMs);
    return MarkedCopy{reply.failure, reply.error};
}

WorkerClient::Inspection WorkerClient::inspect(const QString &path, const QString &coverPath, const QString &convertedPath,
                                               int timeoutMs)
{
    QJsonObject request;
    request.insert(QStringLiteral("op"), QStringLiteral("inspect"));
    request.insert(QStringLiteral("path"), path);
    request.insert(QStringLiteral("coverPath"), coverPath);
    request.insert(QStringLiteral("convertedPath"), convertedPath);
    const Reply reply = call(request, timeoutMs);

    Inspection inspection;
    inspection.failure = reply.failure;
    inspection.error = reply.error;
    if (reply.failure != WorkerFailure::None)
        return inspection;

    const QJsonObject &b = reply.body;
    InspectResult &r = inspection.result;
    r.format = b.value(QStringLiteral("format")).toString();
    r.isKf8 = b.value(QStringLiteral("kf8")).toBool();
    r.encrypted = b.value(QStringLiteral("encrypted")).toBool();
    r.problem = b.value(QStringLiteral("problem")).toString();
    r.blank = b.value(QStringLiteral("blank")).toBool();
    r.coverWritten = b.value(QStringLiteral("coverWritten")).toBool();
    if (b.contains(QStringLiteral("pages")))
        r.pageCount = b.value(QStringLiteral("pages")).toInt();
    if (b.contains(QStringLiteral("hasText")))
        r.hasTextLayer = b.value(QStringLiteral("hasText")).toBool();
    r.largestText = b.value(QStringLiteral("largestText")).toString();
    r.text = b.value(QStringLiteral("text")).toString();

    const QJsonObject m = b.value(QStringLiteral("meta")).toObject();
    r.metadata.title = m.value(QStringLiteral("title")).toString();
    for (const QJsonValue &author : m.value(QStringLiteral("authors")).toArray())
        r.metadata.authors << author.toString();
    r.metadata.publisher = m.value(QStringLiteral("publisher")).toString();
    r.metadata.date = m.value(QStringLiteral("date")).toString();
    r.metadata.language = m.value(QStringLiteral("language")).toString();
    r.metadata.doi = m.value(QStringLiteral("doi")).toString();
    r.metadata.isbn = m.value(QStringLiteral("isbn")).toString();
    r.metadata.issn = m.value(QStringLiteral("issn")).toString();
    r.metadata.description = m.value(QStringLiteral("description")).toString();
    r.metadata.keywords = m.value(QStringLiteral("keywords")).toString();
    return inspection;
}

} // namespace papertrail::import
