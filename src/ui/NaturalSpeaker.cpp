#include "ui/NaturalSpeaker.h"

#include "metadata/NaturalVoices.h"
#include "ui/VoiceClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef PAPERTRAIL_WITH_SPEECH
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>
#endif

namespace papertrail::ui {

namespace {

#ifdef PAPERTRAIL_WITH_SPEECH

// 16-bit mono samples made into what the output takes, where it can't take them as they are.
QByteArray converted(const QByteArray &samples, int rate, const QAudioFormat &to)
{
    const auto *in = reinterpret_cast<const qint16 *>(samples.constData());
    const qsizetype count = samples.size() / qsizetype(sizeof(qint16));
    const int channels = std::max(1, to.channelCount());
    const qsizetype frames = count == 0 ? 0 : qsizetype(double(count) * to.sampleRate() / rate);
    QByteArray out(frames * channels * to.bytesPerSample(), Qt::Uninitialized);
    char *at = out.data();
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const double position = double(frame) * rate / to.sampleRate();
        const qsizetype before = std::min<qsizetype>(qsizetype(position), count - 1);
        const qsizetype after = std::min<qsizetype>(before + 1, count - 1);
        const double fraction = position - double(before);
        const float value = float((in[before] * (1 - fraction) + in[after] * fraction) / 32768.0);
        for (int channel = 0; channel < channels; ++channel) {
            switch (to.sampleFormat()) {
            case QAudioFormat::UInt8:
                *reinterpret_cast<quint8 *>(at) = quint8(std::lround(value * 127.0f + 128.0f));
                break;
            case QAudioFormat::Int16:
                *reinterpret_cast<qint16 *>(at) = qint16(std::lround(value * 32767.0f));
                break;
            case QAudioFormat::Int32:
                *reinterpret_cast<qint32 *>(at) = qint32(std::lround(double(value) * 2147483647.0));
                break;
            case QAudioFormat::Float:
                *reinterpret_cast<float *>(at) = value;
                break;
            default:
                break;
            }
            at += to.bytesPerSample();
        }
    }
    return out;
}

class QtAudioOut : public AudioOut {
public:
    using AudioOut::AudioOut;

    void play(const QByteArray &samples, int sampleRate) override
    {
        stop();
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            emit problem(tr("there's no sound output"));
            return;
        }
        QAudioFormat format;
        format.setSampleRate(sampleRate);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        QByteArray data = samples;
        if (!device.isFormatSupported(format)) {
            format = device.preferredFormat();
            data = converted(samples, sampleRate, format);
        }
        m_buffer = new QBuffer(this);
        m_buffer->setData(data);
        m_buffer->open(QIODevice::ReadOnly);
        m_sink = new QAudioSink(device, format, this);
        m_sink->setVolume(m_volume);
        QAudioSink *sink = m_sink;
        connect(m_sink, &QAudioSink::stateChanged, this, [this, sink](QAudio::State state) {
            if (sink != m_sink)
                return;
            if (state == QAudio::IdleState && m_buffer && m_buffer->atEnd()) {
                stop();
                emit finishedPlaying();
            } else if (state == QAudio::StoppedState && sink->error() != QAudio::NoError) {
                stop();
                emit problem(tr("the sound couldn't be played"));
            }
        });
        m_sink->start(m_buffer);
        if (m_paused)
            m_sink->suspend();
    }

    void pause() override
    {
        m_paused = true;
        if (m_sink)
            m_sink->suspend();
    }

    void resume() override
    {
        m_paused = false;
        if (m_sink)
            m_sink->resume();
    }

    void stop() override
    {
        if (m_sink) {
            QAudioSink *sink = m_sink;
            m_sink = nullptr;
            sink->disconnect(this);
            sink->stop();
            sink->deleteLater();
        }
        if (m_buffer) {
            m_buffer->deleteLater();
            m_buffer = nullptr;
        }
    }

    void setVolume(double volume) override
    {
        m_volume = float(std::clamp(volume, 0.0, 1.0));
        if (m_sink)
            m_sink->setVolume(m_volume);
    }

private:
    QAudioSink *m_sink = nullptr;
    QBuffer *m_buffer = nullptr;
    float m_volume = 1;
    bool m_paused = false;
};

#endif

} // namespace

QString checkNaturalVoice(const QString &workerPath, const QString &voicesDirectory, QString *detail)
{
    QProcess process;
    process.setProgram(workerPath);
    process.setArguments({QStringLiteral("--voice")});
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForStarted(30000))
        return QStringLiteral("the voice's program couldn't be started: %1").arg(process.errorString());
    const auto ask = [&process](const QJsonObject &request, QByteArray *samples) -> QJsonObject {
        process.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        QByteArray line;
        while (!line.endsWith('\n')) {
            if (!process.canReadLine() && !process.waitForReadyRead(60000))
                return {};
            line += process.readLine();
        }
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        const qint64 bytes = qint64(reply.value(QStringLiteral("bytes")).toDouble());
        if (samples) {
            while (process.bytesAvailable() < bytes) {
                if (!process.waitForReadyRead(60000))
                    break;
            }
            *samples = process.read(bytes);
        }
        return reply;
    };
    const QJsonObject version = ask({{QStringLiteral("op"), QStringLiteral("version")}, {QStringLiteral("id"), 1}}, nullptr);
    if (!version.value(QStringLiteral("ok")).toBool())
        return QStringLiteral("the voice's program didn't answer: %1").arg(version.value(QStringLiteral("error")).toString());
    if (detail)
        *detail = QStringLiteral("sherpa-onnx %1").arg(version.value(QStringLiteral("version")).toString());
    if (metadata::naturalVoicesPresent(voicesDirectory)) {
        const metadata::NaturalVoice &voice = metadata::usualNaturalVoice(false);
        QByteArray samples;
        const QJsonObject said = ask({{QStringLiteral("op"), QStringLiteral("say")},
                                      {QStringLiteral("id"), 2},
                                      {QStringLiteral("folder"), metadata::naturalVoicesFolder(voicesDirectory)},
                                      {QStringLiteral("speaker"), voice.speaker},
                                      {QStringLiteral("british"), voice.british},
                                      {QStringLiteral("speed"), 1.0},
                                      {QStringLiteral("text"), QStringLiteral("The beam was measured twice.")}},
                                     &samples);
        if (!said.value(QStringLiteral("ok")).toBool() || samples.isEmpty())
            return QStringLiteral("a sentence couldn't be said: %1").arg(said.value(QStringLiteral("error")).toString());
        if (detail)
            *detail += QStringLiteral(", %1 bytes of sound from %2").arg(samples.size()).arg(QLatin1StringView(voice.name));
    } else if (detail) {
        *detail += QStringLiteral(", with no voices fetched yet");
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(10000))
        process.kill();
    return QString();
}

std::unique_ptr<AudioOut> createAudioOut(QObject *parent)
{
#ifdef PAPERTRAIL_WITH_SPEECH
    return std::make_unique<QtAudioOut>(parent);
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

namespace {

// Sentences made ahead of the one playing.
constexpr int kAhead = 2;
// Sounds kept for going back, about a minute and a half of reading.
constexpr int kSoundsKept = 32;
// A skip this soon after another waits this long before asking for its sentence, so skipping several quickly doesn't
// make the voice say those passed over first.
constexpr int kSettleMs = 250;

} // namespace

NaturalSpeaker::NaturalSpeaker(const QString &workerPath, const QString &voicesFolder, const metadata::NaturalVoice &voice,
                               std::unique_ptr<AudioOut> out, QObject *parent)
    : Speaker(parent), m_client(new VoiceClient(workerPath, this)), m_out(std::move(out)), m_folder(voicesFolder),
      m_speaker(voice.speaker), m_british(voice.british)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    connect(&m_settle, &QTimer::timeout, this, &NaturalSpeaker::playCurrent);
    connect(m_client, &VoiceClient::said, this,
            [this](int id, const QByteArray &samples, int rate) { onAnswered(id, Sound{samples, rate}); });
    connect(m_client, &VoiceClient::failed, this, [this](int id, const QString &) { onAnswered(id, Sound{}); });
    connect(m_client, &VoiceClient::unavailable, this, [this](const QString &message) {
        if (!std::exchange(m_unavailable, true))
            emit unavailable(message);
    });
    if (m_out) {
        connect(m_out.get(), &AudioOut::finishedPlaying, this, [this] {
            m_playing = false;
            ++m_current;
            playCurrent();
        });
        connect(m_out.get(), &AudioOut::problem, this, [this](const QString &message) {
            m_playing = false;
            m_client->cancel();
            m_asked.clear();
            if (!std::exchange(m_unavailable, true))
                emit unavailable(message);
        });
    }
}

NaturalSpeaker::~NaturalSpeaker() = default;

void NaturalSpeaker::say(const QStringList &sentences)
{
    // Saying something else while reading is going back or on, or a new speed: quick ones wait for the last.
    const bool skipping = !m_sentences.isEmpty() || m_settle.isActive();
    stop();
    m_sentences = sentences;
    if (!m_out) {
        if (!std::exchange(m_unavailable, true))
            emit unavailable(tr("there's no sound output"));
        return;
    }
    if (skipping && !m_sentences.isEmpty() && !m_sounds.contains(keyFor(0)))
        m_settle.start();
    playCurrent();
}

void NaturalSpeaker::prepare(const QStringList &upcoming)
{
    m_upcoming = upcoming;
    if (!m_sentences.isEmpty() && !m_settle.isActive())
        askAhead();
}

QString NaturalSpeaker::keyFor(const QString &sentence) const
{
    return QString::number(m_speed) + u'\n' + sentence;
}

void NaturalSpeaker::ask(const QString &sentence)
{
    const QString key = keyFor(sentence);
    if (m_sounds.contains(key) || std::find(m_asked.cbegin(), m_asked.cend(), key) != m_asked.cend())
        return;
    m_asked.insert(m_client->say(m_folder, m_speaker, m_british, m_speed, sentence), key);
}

void NaturalSpeaker::askAhead()
{
    // In order, so the sentence wanted first is made first, and on into what's likely to come after.
    const int last = m_current + kAhead;
    for (int index = m_current; index <= last && index < m_sentences.size(); ++index)
        ask(m_sentences.at(index));
    for (int index = int(m_sentences.size()); index <= last && index - m_sentences.size() < m_upcoming.size(); ++index)
        ask(m_upcoming.at(index - m_sentences.size()));
}

void NaturalSpeaker::playCurrent()
{
    if (m_playing || m_paused || m_unavailable)
        return;
    // Past any sentence given up on.
    while (m_current < m_sentences.size()) {
        const auto found = m_sounds.constFind(keyFor(m_current));
        if (found == m_sounds.cend() || !found->samples.isEmpty())
            break;
        ++m_current;
    }
    if (m_current >= m_sentences.size()) {
        if (!m_sentences.isEmpty()) {
            m_sentences.clear();
            emit finished();
        }
        return;
    }
    if (!m_settle.isActive())
        askAhead();
    const auto found = m_sounds.constFind(keyFor(m_current));
    if (found == m_sounds.cend())
        return; // It's asked for, or will be once skipping settles, and plays when it arrives.
    m_settle.stop();
    const Sound sound = *found;
    m_playing = true;
    emit sentenceStarted(m_current);
    m_out->play(sound.samples, sound.rate);
}

void NaturalSpeaker::keep(const QString &key, const Sound &sound)
{
    if (!m_sounds.contains(key))
        m_kept << key;
    m_sounds.insert(key, sound);
    while (m_kept.size() > kSoundsKept)
        m_sounds.remove(m_kept.takeFirst());
}

void NaturalSpeaker::onAnswered(int id, const Sound &sound)
{
    const auto found = m_asked.constFind(id);
    if (found == m_asked.cend())
        return;
    const QString key = found.value();
    m_asked.erase(found);
    // Kept even when it's no longer wanted, since it may be gone back to.
    keep(key, sound);
    playCurrent();
}

void NaturalSpeaker::forgetWaiting()
{
    for (const int id : m_client->cancelWaiting())
        m_asked.remove(id);
}

void NaturalSpeaker::pause()
{
    m_paused = true;
    if (m_out)
        m_out->pause();
}

void NaturalSpeaker::resume()
{
    m_paused = false;
    if (m_out)
        m_out->resume();
    playCurrent();
}

void NaturalSpeaker::stop()
{
    // The sentence being made is still answered, and kept.
    forgetWaiting();
    m_settle.stop();
    if (m_out)
        m_out->stop();
    m_sentences.clear();
    m_upcoming.clear();
    m_current = 0;
    m_playing = false;
    m_paused = false;
}

void NaturalSpeaker::setRate(double rate)
{
    const double speed = 1.0 + 0.6 * std::clamp(rate, -1.0, 1.0);
    if (speed == m_speed)
        return;
    m_speed = speed;
    // What's kept at the old speed won't be played again.
    m_sounds.clear();
    m_kept.clear();
}

void NaturalSpeaker::setVolume(double volume)
{
    if (m_out)
        m_out->setVolume(volume);
}

} // namespace papertrail::ui
