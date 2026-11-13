#include "ui/VoiceClient.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>

namespace papertrail::ui {

namespace {

// A sentence that stops the process this many times is given up on.
constexpr int kMostAttempts = 2;
// Five minutes unused, and the voices' memory is given back.
constexpr int kIdleMs = 5 * 60 * 1000;

} // namespace

VoiceClient::VoiceClient(const QString &workerPath, QObject *parent)
    : QObject(parent), m_workerPath(workerPath)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        // The finished signal takes it from here, as for a crash.
        if (m_process)
            m_process->kill();
    });
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleMs);
    connect(&m_idle, &QTimer::timeout, this, &VoiceClient::stopProcess);
}

VoiceClient::~VoiceClient()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void VoiceClient::setTimeouts(int sayMs, int idleMs)
{
    m_sayMs = sayMs;
    m_idle.setInterval(idleMs);
}

bool VoiceClient::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

int VoiceClient::say(const QString &folder, int speaker, bool british, double speed, const QString &text)
{
    Request request;
    request.id = m_nextId++;
    const QJsonObject body{{QStringLiteral("op"), QStringLiteral("say")},
                           {QStringLiteral("id"), request.id},
                           {QStringLiteral("folder"), folder},
                           {QStringLiteral("speaker"), speaker},
                           {QStringLiteral("british"), british},
                           {QStringLiteral("speed"), speed},
                           {QStringLiteral("text"), text}};
    request.line = QJsonDocument(body).toJson(QJsonDocument::Compact) + '\n';
    if (m_unavailable) {
        const int id = request.id;
        QMetaObject::invokeMethod(this, [this, id] { emit failed(id, tr("the natural voice can't be used here")); },
                                  Qt::QueuedConnection);
        return id;
    }
    m_queue << request;
    m_idle.stop();
    if (!m_process)
        startProcess();
    pump();
    return request.id;
}

void VoiceClient::cancel()
{
    m_queue.clear();
    if (m_inFlight)
        m_dropInFlight = true;
}

QList<int> VoiceClient::cancelWaiting()
{
    QList<int> ids;
    for (const Request &request : std::as_const(m_queue))
        ids << request.id;
    m_queue.clear();
    if (!m_inFlight)
        m_idle.start();
    return ids;
}

void VoiceClient::startProcess()
{
    if (!QFileInfo(m_workerPath).isExecutable()) {
        m_unavailable = true;
        const QList<Request> lost = m_queue;
        m_queue.clear();
        emit unavailable(tr("Papertrail's helper program is missing"));
        for (const Request &request : lost)
            emit failed(request.id, tr("the natural voice can't be used here"));
        return;
    }
    m_process = new QProcess(this);
    m_process->setProgram(m_workerPath);
    m_process->setArguments({QStringLiteral("--voice")});
    m_process->setStandardErrorFile(QProcess::nullDevice());
    connect(m_process, &QProcess::readyReadStandardOutput, this, &VoiceClient::onReadyRead);
    connect(m_process, &QProcess::finished, this, &VoiceClient::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onProcessFinished();
    });
    m_buffer.clear();
    m_pendingHeader.reset();
    m_pendingBytes = 0;
    m_process->start(QIODevice::ReadWrite);
}

void VoiceClient::pump()
{
    if (m_inFlight || !m_process || m_queue.isEmpty())
        return;
    m_inFlight = m_queue.takeFirst();
    m_dropInFlight = false;
    m_process->write(m_inFlight->line);
    m_timeout.start(m_sayMs);
}

void VoiceClient::onReadyRead()
{
    m_buffer += m_process->readAllStandardOutput();
    while (true) {
        if (!m_pendingHeader) {
            const qsizetype end = m_buffer.indexOf('\n');
            if (end < 0)
                return;
            const QJsonDocument header = QJsonDocument::fromJson(m_buffer.left(end));
            m_buffer.remove(0, end + 1);
            if (!header.isObject())
                continue;
            m_pendingHeader = header.object();
            m_pendingBytes = qint64(m_pendingHeader->value(QStringLiteral("bytes")).toDouble());
        }
        if (m_buffer.size() < m_pendingBytes)
            return;
        const QByteArray samples = m_buffer.left(m_pendingBytes);
        m_buffer.remove(0, m_pendingBytes);
        const QJsonObject reply = *m_pendingHeader;
        m_pendingHeader.reset();
        m_pendingBytes = 0;

        if (!m_inFlight || reply.value(QStringLiteral("id")).toInt() != m_inFlight->id)
            continue;
        m_timeout.stop();
        const int id = m_inFlight->id;
        const bool dropped = m_dropInFlight;
        m_inFlight.reset();
        m_dropInFlight = false;
        if (!dropped) {
            if (reply.value(QStringLiteral("ok")).toBool())
                emit said(id, samples, reply.value(QStringLiteral("rate")).toInt());
            else
                emit failed(id, reply.value(QStringLiteral("error")).toString());
        }
        if (m_queue.isEmpty())
            m_idle.start();
        pump();
    }
}

void VoiceClient::onProcessFinished()
{
    m_timeout.stop();
    QProcess *gone = m_process;
    m_process = nullptr;
    const bool neverStarted = gone && gone->error() == QProcess::FailedToStart;
    if (gone) {
        gone->disconnect(this);
        gone->deleteLater();
    }
    if (neverStarted) {
        m_unavailable = true;
        QList<Request> lost = m_queue;
        if (m_inFlight && !m_dropInFlight)
            lost.prepend(*m_inFlight);
        m_inFlight.reset();
        m_queue.clear();
        emit unavailable(tr("the natural voice's program couldn't be started"));
        for (const Request &request : lost)
            emit failed(request.id, tr("the natural voice can't be used here"));
        return;
    }
    // What was being said when it stopped is tried again, once.
    if (m_inFlight) {
        Request lost = *m_inFlight;
        const bool dropped = m_dropInFlight;
        m_inFlight.reset();
        m_dropInFlight = false;
        if (!dropped) {
            if (++lost.attempts >= kMostAttempts)
                emit failed(lost.id, tr("this sentence made the voice stop"));
            else
                m_queue.prepend(lost);
        }
    }
    if (!m_queue.isEmpty()) {
        startProcess();
        pump();
    }
}

void VoiceClient::stopProcess()
{
    if (!m_process || m_inFlight || !m_queue.isEmpty())
        return;
    // Closing its input ends it once it's read everything, which it has.
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    process->closeWriteChannel();
}

} // namespace papertrail::ui
