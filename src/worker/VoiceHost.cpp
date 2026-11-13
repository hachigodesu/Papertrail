#include "VoiceHost.h"

#include "ReaderHost.h"

#ifdef PAPERTRAIL_WITH_NATURAL_VOICE
#include "voice/Voice.h"
#endif

#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace papertrail::worker {

namespace {

// Kokoro's English voices. Others in the model aren't offered.
constexpr int kSpeakers = 28;
constexpr int kToneRate = 24000;
// More would be longer than any sentence on a page, and is likely something else.
constexpr qsizetype kLongestText = 4000;

void reply(FILE *out, QJsonObject header, const std::vector<qint16> &samples = {})
{
    const qint64 bytes = qint64(samples.size() * sizeof(qint16));
    if (bytes > 0)
        header.insert(QStringLiteral("bytes"), double(bytes));
    const QByteArray line = QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), out);
    if (bytes > 0)
        std::fwrite(samples.data(), 1, size_t(bytes), out);
    std::fflush(out);
}

void applyFaults(const QString &text)
{
    static const bool enabled = qEnvironmentVariableIsSet("PAPERTRAIL_WORKER_TEST_FAULTS");
    if (!enabled)
        return;
    if (text.contains(QStringLiteral("__crash__")))
        std::abort();
    if (text.contains(QStringLiteral("__hang__"))) {
        while (true)
            QThread::sleep(60);
    }
}

// A short tone for each sentence, a few milliseconds a letter, for tests that mustn't need the model.
std::vector<qint16> tone(const QString &text, double speed)
{
    const qsizetype count = qMax<qsizetype>(kToneRate / 25, qsizetype(kToneRate * 0.004 * text.size() / qMax(0.25, speed)));
    std::vector<qint16> samples(static_cast<size_t>(count));
    for (qsizetype i = 0; i < count; ++i)
        samples[size_t(i)] = qint16(3000 * std::sin(2 * std::numbers::pi * 440 * double(i) / kToneRate));
    return samples;
}

} // namespace

int runVoiceHost()
{
    FILE *out = claimStandardOutput();
    if (!out)
        return 1;
    const bool testTone = qEnvironmentVariableIsSet("PAPERTRAIL_VOICE_TEST_TONE");
#ifdef PAPERTRAIL_WITH_NATURAL_VOICE
    voice::Voice voice;
    const int threads = qBound(1, QThread::idealThreadCount(), 4);
#endif
    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString op = request.value(QStringLiteral("op")).toString();
        QJsonObject header{{QStringLiteral("id"), request.value(QStringLiteral("id"))}};
        const auto fail = [&](const QString &error) {
            header.insert(QStringLiteral("ok"), false);
            header.insert(QStringLiteral("error"), error);
            reply(out, header);
        };
#ifndef PAPERTRAIL_WITH_NATURAL_VOICE
        if (!testTone) {
            fail(QStringLiteral("this copy of Papertrail can't use natural voices"));
            continue;
        }
#endif
        if (op == QLatin1StringView("version")) {
            header.insert(QStringLiteral("ok"), true);
#ifdef PAPERTRAIL_WITH_NATURAL_VOICE
            header.insert(QStringLiteral("version"), voice::version());
#else
            header.insert(QStringLiteral("version"), QStringLiteral("test tone"));
#endif
            reply(out, header);
            continue;
        }
        if (op != QLatin1StringView("say")) {
            fail(QStringLiteral("unknown request"));
            continue;
        }
        const QString text = request.value(QStringLiteral("text")).toString();
        const int speaker = request.value(QStringLiteral("speaker")).toInt(-1);
        const double speed = request.value(QStringLiteral("speed")).toDouble(1.0);
        if (speaker < 0 || speaker >= kSpeakers) {
            fail(QStringLiteral("there's no such voice"));
            continue;
        }
        if (text.trimmed().isEmpty() || text.size() > kLongestText) {
            fail(QStringLiteral("there's nothing to say"));
            continue;
        }
        applyFaults(text);
        std::vector<qint16> samples;
        int rate = kToneRate;
        if (testTone) {
            samples = tone(text, speed);
        } else {
#ifdef PAPERTRAIL_WITH_NATURAL_VOICE
            const QString folder = request.value(QStringLiteral("folder")).toString();
            const bool british = request.value(QStringLiteral("british")).toBool();
            if (!voice.isLoaded() || voice.folder() != folder || voice.british() != british) {
                const QString error = voice.load(folder, british, threads);
                if (!error.isEmpty()) {
                    fail(error);
                    continue;
                }
            }
            QString error;
            samples = voice.say(text, speaker, speed, &error);
            rate = voice.sampleRate();
            if (samples.empty()) {
                fail(error);
                continue;
            }
#endif
        }
        header.insert(QStringLiteral("ok"), true);
        header.insert(QStringLiteral("rate"), rate);
        reply(out, header, samples);
    }
    return 0;
}

} // namespace papertrail::worker
