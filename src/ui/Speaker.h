#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace papertrail::ui {

// Says sentences aloud, one after another, saying which it's starting. Tests use one that only pretends.
class Speaker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Replaces anything being said with these, in order.
    virtual void say(const QStringList &sentences) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual bool isPaused() const = 0;
    // From -1, slowest, to 1, fastest. 0 is the voice's own pace.
    virtual void setRate(double rate) = 0;

signals:
    // The sentence at this index of those given to say() is starting.
    void sentenceStarted(int index);
    // All of them have been said.
    void finished();
    void problem(const QString &message);
};

// Whether this build can read aloud.
bool canReadAloud();

// Qt's text-to-speech, with the voice chosen in reader.voice if there's one, on the engine best for this system:
// Windows' own voices, or on Linux speech-dispatcher's if it has any, else flite's, which comes with Papertrail.
std::unique_ptr<Speaker> createSystemSpeaker(const QString &voice, QObject *parent = nullptr);

// The voices there are, for Settings.
QStringList availableVoices();

// For the self-test: whether a sentence can be made into sound here, without playing it. Empty if so, or what's wrong,
// with `detail` saying which engine and voice.
QString checkSpeech(QString *detail);

} // namespace papertrail::ui
