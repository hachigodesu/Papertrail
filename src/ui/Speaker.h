#pragma once

#include <QLocale>
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
    // What's likely to be said once those given to say() are, such as the next page's first sentences, which a voice
    // that takes a while to make a sentence can start on. Forgotten on the next say() or stop().
    virtual void prepare(const QStringList &upcoming) { Q_UNUSED(upcoming); }
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual bool isPaused() const = 0;
    // From -1, slowest, to 1, fastest. 0 is the voice's own pace. Heard from the next sentence said.
    virtual void setRate(double rate) = 0;
    // From 0, silent, to 1, as loud as it goes.
    virtual void setVolume(double volume) = 0;

signals:
    // The sentence at this index of those given to say() is starting.
    void sentenceStarted(int index);
    // All of them have been said.
    void finished();
    void problem(const QString &message);
    // It can't work here at all, such as a natural voice with no sound output, so another should take over.
    void unavailable(const QString &message);
};

// Whether this build can read aloud.
bool canReadAloud();
// Whether it can read with Kokoro's natural voices, once they've been fetched.
bool canSpeakNaturally();

// Qt's text-to-speech, with the voice chosen in reader.voice if there's one, on the engine best for this system:
// Windows' own voices, or on Linux speech-dispatcher's if it has any, else flite's, which comes with Papertrail.
std::unique_ptr<Speaker> createSystemSpeaker(const QString &voice, QObject *parent = nullptr);

// The voices there are, for Settings.
QStringList availableVoices();

// reader.voice for a natural voice: this, then Kokoro's id, such as "kokoro:bf_emma".
inline constexpr QLatin1StringView kNaturalVoicePrefix{"kokoro:"};
// What a natural voice is called, such as "Emma (British English, natural)", or nothing for an id that isn't one.
QString naturalVoiceLabel(const QString &id);
// The natural voice that reads for reader.voice, by Kokoro's id, or nothing for the system's. The usual voice is a
// natural one once they're here, British for a British locale.
QString naturalVoiceFor(const QString &setting, bool voicesHere, const QLocale &locale);

// For the self-test: whether a sentence can be made into sound here, without playing it. Empty if so, or what's wrong,
// with `detail` saying which engine and voice.
QString checkSpeech(QString *detail);

} // namespace papertrail::ui
