#include "voice/Voice.h"

#include "sherpa-onnx/c-api/c-api.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace papertrail::voice {

struct Voice::Engine {
    const SherpaOnnxOfflineTts *tts = nullptr;
    ~Engine()
    {
        if (tts)
            SherpaOnnxDestroyOfflineTts(tts);
    }
};

QString version()
{
    return QString::fromLatin1(SherpaOnnxGetVersionStr());
}

Voice::Voice() = default;
Voice::~Voice() = default;

QString Voice::load(const QString &folder, bool british, int threads)
{
    m_engine.reset();
    m_folder.clear();
    const QDir dir(folder);
    const QString lexiconName = british ? QStringLiteral("lexicon-gb-en.txt") : QStringLiteral("lexicon-us-en.txt");
    for (const QString &needed : {QStringLiteral("model.onnx"), QStringLiteral("voices.bin"), QStringLiteral("tokens.txt"),
                                  lexiconName, QStringLiteral("espeak-ng-data/phontab")}) {
        if (!QFileInfo(dir.filePath(needed)).isFile())
            return QStringLiteral("%1 is missing").arg(needed);
    }
    // sherpa-onnx opens files by narrow path, which on Windows is the system's code page rather than UTF-8, so a folder
    // named in other letters couldn't be reached. This process does nothing else, so it works from inside the folder,
    // and gives sherpa-onnx only the files' own names, which are plain ASCII.
    if (!QDir::setCurrent(folder))
        return QStringLiteral("the voices' folder couldn't be opened");
    const std::string lexicon = lexiconName.toStdString();

    SherpaOnnxOfflineTtsConfig config;
    std::memset(&config, 0, sizeof config);
    config.model.kokoro.model = "model.onnx";
    config.model.kokoro.voices = "voices.bin";
    config.model.kokoro.tokens = "tokens.txt";
    config.model.kokoro.lexicon = lexicon.c_str();
    config.model.kokoro.data_dir = "espeak-ng-data";
    config.model.kokoro.lang = "en";
    config.model.kokoro.length_scale = 1.0f;
    config.model.num_threads = std::max(1, threads);
    config.model.provider = "cpu";
    config.max_num_sentences = 1;

    auto engine = std::make_unique<Engine>();
    engine->tts = SherpaOnnxCreateOfflineTts(&config);
    if (!engine->tts)
        return QStringLiteral("the voices couldn't be loaded");
    m_engine = std::move(engine);
    m_folder = folder;
    m_british = british;
    return QString();
}

bool Voice::isLoaded() const
{
    return m_engine != nullptr;
}

int Voice::sampleRate() const
{
    return m_engine ? SherpaOnnxOfflineTtsSampleRate(m_engine->tts) : 0;
}

std::vector<qint16> Voice::say(const QString &text, int speaker, double speed, QString *error)
{
    std::vector<qint16> samples;
    if (!m_engine) {
        if (error)
            *error = QStringLiteral("the voices aren't loaded");
        return samples;
    }
    const std::string utf8 = text.toStdString();
    // The simplest call, which 1.13.8 says is giving way to one with more settings than Kokoro uses.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const SherpaOnnxGeneratedAudio *audio =
        SherpaOnnxOfflineTtsGenerate(m_engine->tts, utf8.c_str(), speaker, float(std::clamp(speed, 0.25, 4.0)));
#pragma GCC diagnostic pop
    if (!audio || audio->n <= 0) {
        if (audio)
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        if (error)
            *error = QStringLiteral("the sentence couldn't be said");
        return samples;
    }
    samples.resize(size_t(audio->n));
    for (int32_t i = 0; i < audio->n; ++i)
        samples[size_t(i)] = qint16(std::lround(std::clamp(audio->samples[i], -1.0f, 1.0f) * 32767.0f));
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    return samples;
}

} // namespace papertrail::voice
