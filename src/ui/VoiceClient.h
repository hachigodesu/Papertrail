#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <optional>

class QProcess;

namespace papertrail::ui {

// Drives a papertrail-worker --voice process, which says sentences with the natural voices, so nothing in the window
// ever runs the voice itself. It asks for one sentence at a time, in order, and restarts the process if it stops. A
// sentence that stops it twice is given up on. The process quits once it's been unused a while, giving back the memory
// the voices take.
class VoiceClient : public QObject {
    Q_OBJECT

public:
    explicit VoiceClient(const QString &workerPath, QObject *parent = nullptr);
    ~VoiceClient() override;

    // How long a sentence may take, the first including loading the voices, and how long unused before the process
    // quits. Only tests change these.
    void setTimeouts(int sayMs, int idleMs);

    // Asks for a sentence, by the voice's number in the model. Answered by said() or failed() with the id returned.
    int say(const QString &folder, int speaker, bool british, double speed, const QString &text);
    // Forgets every sentence asked for. Answers still coming for them are dropped.
    void cancel();
    // Forgets the sentences waiting to be made, returning their ids, but not the one being made, which is still
    // answered: the process can't be stopped mid-sentence without loading the voices again.
    QList<int> cancelWaiting();
    bool isRunning() const;

signals:
    // 16-bit mono samples at sampleRate.
    void said(int id, const QByteArray &samples, int sampleRate);
    void failed(int id, const QString &error);
    // The process can't be started at all, so no natural voice can be used here.
    void unavailable(const QString &error);

private:
    struct Request {
        int id = 0;
        QByteArray line;
        int attempts = 0;
    };

    void startProcess();
    void pump();
    void onReadyRead();
    void onProcessFinished();
    void stopProcess();

    QString m_workerPath;
    QProcess *m_process = nullptr;
    QList<Request> m_queue;
    std::optional<Request> m_inFlight;
    bool m_dropInFlight = false;
    QByteArray m_buffer;
    std::optional<QJsonObject> m_pendingHeader;
    qint64 m_pendingBytes = 0;
    int m_nextId = 1;
    bool m_unavailable = false;
    QTimer m_timeout;
    QTimer m_idle;
    int m_sayMs = 60000;
};

} // namespace papertrail::ui
