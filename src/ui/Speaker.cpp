#include "ui/Speaker.h"

#include "metadata/NaturalVoices.h"

#include <QCoreApplication>

#ifdef PAPERTRAIL_WITH_SPEECH
#include <QAudioBuffer>
#include <QEventLoop>
#include <QLocale>
#include <QTextToSpeech>
#include <QTimer>
#include <QVoice>
#endif

#ifdef PAPERTRAIL_WITH_FLITE
extern "C" {
#include <flite/flite.h>
cst_voice *register_cmu_us_slt(const char *voxdir);
}
namespace {
// A use of the voice's library that the compiler must keep, so linking records it even where only libraries in use are
// linked (`--as-needed`, as Ubuntu's linker does by default), and packages bundle it. A reference nothing read was
// optimised away in release builds, so the AppImage had no voice.
[[gnu::used]] cst_voice *(*const keepVoiceLinked)(const char *) = &register_cmu_us_slt;
} // namespace
#endif

namespace papertrail::ui {

bool canReadAloud()
{
#ifdef PAPERTRAIL_WITH_SPEECH
    return true;
#else
    return false;
#endif
}

#ifdef PAPERTRAIL_WITH_SPEECH

namespace {

// Qt's flite engine only looks for voices' libraries in the system's own folders, never where a Flatpak or AppImage
// keeps its own, but also goes by the names of voices already listed in the process. For each name it loads the
// voice's library by its soname, registers the voice, and unregisters it, freeing it, when the engine goes. So only a
// stand-in with the voice's name is listed here: listing the voice itself left the list pointing at a freed voice once
// an engine had gone, and the next engine made crashed. The voice's library is linked into Papertrail, so it's already
// loaded when the engine asks for it by name, wherever it was found.
void registerBundledVoice()
{
#ifdef PAPERTRAIL_WITH_FLITE
    static const bool registered = [] {
        flite_init();
        cst_voice *listed = new_voice();
        listed->name = "slt";
        flite_add_voice(listed);
        return true;
    }();
    Q_UNUSED(registered);
#endif
}

// Waits for an engine to be ready, which some are as soon as they're made, and others a moment later.
bool waitUntilReady(QTextToSpeech &speech, int timeoutMs)
{
    if (speech.state() == QTextToSpeech::Ready)
        return true;
    if (speech.state() == QTextToSpeech::Error)
        return false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&speech, &QTextToSpeech::stateChanged, &loop, [&](QTextToSpeech::State state) {
        if (state == QTextToSpeech::Ready || state == QTextToSpeech::Error)
            loop.quit();
    });
    timeout.start(timeoutMs);
    loop.exec();
    return speech.state() == QTextToSpeech::Ready;
}

// An engine with a voice, in order of preference, made ready.
std::unique_ptr<QTextToSpeech> bestEngine(QObject *parent)
{
    registerBundledVoice();
    const QStringList available = QTextToSpeech::availableEngines();
    QStringList order;
#ifdef Q_OS_WIN
    order << QStringLiteral("winrt") << QStringLiteral("sapi");
#else
    order << QStringLiteral("speechd") << QStringLiteral("flite");
#endif
    for (const QString &engine : std::as_const(order)) {
        if (!available.contains(engine))
            continue;
        auto speech = std::make_unique<QTextToSpeech>(engine, parent);
        if (!waitUntilReady(*speech, 5000))
            continue;
        // A locale it has no voice for, such as "C", leaves it without one: English is what's sure to be there.
        if (speech->availableVoices().isEmpty() && !speech->availableLocales().isEmpty())
            speech->setLocale(speech->availableLocales().contains(QLocale(QLocale::English, QLocale::UnitedStates))
                                  ? QLocale(QLocale::English, QLocale::UnitedStates)
                                  : speech->availableLocales().first());
        if (!speech->availableVoices().isEmpty())
            return speech;
    }
    return nullptr;
}

class SystemSpeaker : public Speaker {
public:
    SystemSpeaker(const QString &voice, QObject *parent) : Speaker(parent), m_speech(bestEngine(this))
    {
        if (!m_speech)
            return;
        for (const QVoice &each : m_speech->availableVoices()) {
            if (each.name() == voice)
                m_speech->setVoice(each);
        }
        // One sentence at a time, the next once the engine's ready again. Engines say they're about to make each queued
        // sentence into sound well before it's heard, so that can't say which is being read.
        // A sentence is only done once it's been heard: stopping one to start another says the engine is ready a moment
        // later, after the next has been given to it, and taking that for the next's end ran the marking ahead of the
        // voice.
        connect(m_speech.get(), &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State state) {
            if (state == QTextToSpeech::Error) {
                m_saying = false;
                emit problem(m_speech->errorString());
            } else if (state == QTextToSpeech::Speaking || state == QTextToSpeech::Synthesizing) {
                if (m_saying)
                    m_heard = true;
            } else if (state == QTextToSpeech::Ready && m_saying && m_heard && !m_stopping) {
                m_saying = false;
                m_heard = false;
                ++m_next;
                // After this has returned, so whoever's told can stop or start again.
                QTimer::singleShot(0, this, &SystemSpeaker::sayNext);
            }
        });
    }

    void say(const QStringList &sentences) override
    {
        if (!m_speech) {
            emit problem(tr("No voice is available on this computer."));
            return;
        }
        stop();
        m_sentences = sentences;
        m_next = 0;
        ++m_round;
        sayNext();
    }

    void pause() override
    {
        if (m_speech)
            m_speech->pause();
    }

    void resume() override
    {
        if (m_speech)
            m_speech->resume();
    }

    void stop() override
    {
        m_sentences.clear();
        m_saying = false;
        m_heard = false;
        m_stopping = true;
        if (m_speech)
            m_speech->stop();
        m_stopping = false;
    }

    bool isPaused() const override { return m_speech && m_speech->state() == QTextToSpeech::Paused; }

    void setRate(double rate) override
    {
        if (m_speech)
            m_speech->setRate(qBound(-1.0, rate, 1.0));
    }

    void setVolume(double volume) override
    {
        if (m_speech)
            m_speech->setVolume(qBound(0.0, volume, 1.0));
    }

private:
    void sayNext()
    {
        if (m_saying || m_sentences.isEmpty())
            return;
        if (m_next >= m_sentences.size()) {
            m_sentences.clear();
            emit finished();
            return;
        }
        const int round = m_round;
        emit sentenceStarted(m_next);
        if (round != m_round || m_sentences.isEmpty())
            return; // Stopped, or given others, by whoever was told.
        m_saying = true;
        m_heard = false;
        m_speech->say(m_sentences.at(m_next));
    }

    std::unique_ptr<QTextToSpeech> m_speech;
    QStringList m_sentences;
    int m_next = 0;
    int m_round = 0;
    bool m_saying = false;
    bool m_heard = false; // The sentence being said has been heard speaking, so its engine being ready means it's done.
    bool m_stopping = false;
};

} // namespace

std::unique_ptr<Speaker> createSystemSpeaker(const QString &voice, QObject *parent)
{
    return std::make_unique<SystemSpeaker>(voice, parent);
}

QStringList availableVoices()
{
    QStringList names;
    if (const std::unique_ptr<QTextToSpeech> speech = bestEngine(nullptr)) {
        for (const QVoice &voice : speech->availableVoices())
            names << voice.name();
    }
    return names;
}

QString checkSpeech(QString *detail)
{
    const std::unique_ptr<QTextToSpeech> speech = bestEngine(nullptr);
    if (!speech)
        return QStringLiteral("no engine has a voice (engines: %1)").arg(QTextToSpeech::availableEngines().join(QStringLiteral(", ")));
    if (detail)
        *detail = QStringLiteral("%1, %2").arg(speech->engine(), speech->voice().name());
    if (!(speech->engineCapabilities() & QTextToSpeech::Capability::Synthesize))
        return QString(); // It has a voice, and can only be heard, not checked.
    qint64 bytes = 0;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(speech.get(), &QTextToSpeech::stateChanged, &loop, [&](QTextToSpeech::State state) {
        if (state == QTextToSpeech::Ready || state == QTextToSpeech::Error)
            loop.quit();
    });
    speech->synthesize(QStringLiteral("The beam was measured twice."), speech.get(),
                       [&bytes](const QAudioBuffer &buffer) { bytes += buffer.byteCount(); });
    if (bytes == 0) {
        timeout.start(30000);
        loop.exec();
    }
    if (bytes == 0)
        return QStringLiteral("a sentence couldn't be made into sound: %1").arg(speech->errorString());
    if (detail)
        *detail += QStringLiteral(", %1 bytes of sound").arg(bytes);
    return QString();
}

#else

std::unique_ptr<Speaker> createSystemSpeaker(const QString &, QObject *)
{
    return nullptr;
}

QStringList availableVoices()
{
    return {};
}

QString checkSpeech(QString *)
{
    return QStringLiteral("this build can't read aloud");
}

#endif

bool canSpeakNaturally()
{
#ifdef PAPERTRAIL_WITH_NATURAL_VOICE
    return true;
#else
    return false;
#endif
}

QString naturalVoiceFor(const QString &setting, bool voicesHere, const QLocale &locale)
{
    if (!canSpeakNaturally() || !voicesHere)
        return QString();
    if (setting.isEmpty())
        return QLatin1StringView(metadata::usualNaturalVoice(locale.territory() == QLocale::UnitedKingdom).id);
    if (setting.startsWith(kNaturalVoicePrefix)) {
        const QString id = setting.mid(kNaturalVoicePrefix.size());
        if (metadata::naturalVoice(id))
            return id;
    }
    return QString();
}

QString naturalVoiceLabel(const QString &id)
{
    const metadata::NaturalVoice *voice = metadata::naturalVoice(id);
    if (!voice)
        return QString();
    return voice->british ? QCoreApplication::translate("Speaker", "%1 (British English, natural)").arg(QLatin1StringView(voice->name))
                          : QCoreApplication::translate("Speaker", "%1 (American English, natural)").arg(QLatin1StringView(voice->name));
}

} // namespace papertrail::ui
