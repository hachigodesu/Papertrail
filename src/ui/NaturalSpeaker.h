#pragma once

#include "ui/Speaker.h"

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace papertrail::metadata {
struct NaturalVoice;
}

namespace papertrail::ui {

class VoiceClient;

// Plays sound. Tests use one that only pretends, so no test makes a sound.
class AudioOut : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Plays 16-bit mono samples at `sampleRate` in place of anything playing, then says finishedPlaying().
    virtual void play(const QByteArray &samples, int sampleRate) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    // From 0, silent, to 1.
    virtual void setVolume(double volume) = 0;

signals:
    void finishedPlaying();
    // Nothing can be played here, such as with no sound output.
    void problem(const QString &message);
};

// Through the computer's usual sound output with Qt Multimedia. Null in a build that can't read aloud.
std::unique_ptr<AudioOut> createAudioOut(QObject *parent = nullptr);

// For the self-test: whether the worker's voice process starts and says which sherpa-onnx it has, and, where the
// voices have been fetched to `voicesDirectory`, makes a sentence into sound without playing it. Empty if so, or what's
// wrong, with `detail` saying what was found.
QString checkNaturalVoice(const QString &workerPath, const QString &voicesDirectory, QString *detail);

// Reads with one of Kokoro's natural voices, made a sentence at a time by a papertrail-worker --voice process, the next
// two sentences asked for while one plays. A sentence is started, and said to be, as its sound starts. Sounds made are
// kept a while, so going back plays at once, and going on to one made ahead does too.
class NaturalSpeaker : public Speaker {
    Q_OBJECT

public:
    NaturalSpeaker(const QString &workerPath, const QString &voicesFolder, const metadata::NaturalVoice &voice,
                   std::unique_ptr<AudioOut> out, QObject *parent = nullptr);
    ~NaturalSpeaker() override;

    void say(const QStringList &sentences) override;
    void prepare(const QStringList &upcoming) override;
    void pause() override;
    void resume() override;
    void stop() override;
    bool isPaused() const override { return m_paused; }
    // -1 to 1 is Kokoro's speeds from 0.6 to 1.6.
    void setRate(double rate) override;
    void setVolume(double volume) override;

    VoiceClient *client() const { return m_client; }
    double speed() const { return m_speed; }
    // How long a skip waits for another before asking for its sentence. Only tests change it.
    void setSettleMs(int ms) { m_settle.setInterval(ms); }

private:
    struct Sound {
        QByteArray samples; // Empty for a sentence given up on.
        int rate = 0;
    };

    QString keyFor(const QString &sentence) const;
    QString keyFor(int index) const { return keyFor(m_sentences.at(index)); }
    void ask(const QString &sentence);
    void askAhead();
    void playCurrent();
    void keep(const QString &key, const Sound &sound);
    void onAnswered(int id, const Sound &sound);
    void forgetWaiting();

    VoiceClient *m_client;
    std::unique_ptr<AudioOut> m_out;
    QString m_folder;
    int m_speaker;
    bool m_british;
    QStringList m_sentences;
    QStringList m_upcoming;      // Likely to be said after them.
    int m_current = 0;           // The sentence playing, or to play next.
    QHash<int, QString> m_asked; // What's asked for and not yet answered, by id.
    QHash<QString, Sound> m_sounds; // Sounds made, by what was said at what speed.
    QStringList m_kept;          // Their keys, oldest first.
    QTimer m_settle;
    bool m_playing = false;
    bool m_paused = false;
    bool m_unavailable = false;
    double m_speed = 1;
};

} // namespace papertrail::ui
