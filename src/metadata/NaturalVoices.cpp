#include "metadata/NaturalVoices.h"

#include <QDir>
#include <QFileInfo>

namespace papertrail::metadata {

namespace {

// sherpa-onnx's copy of Kokoro 1.0 at this commit, which never changes.
constexpr const char *kCommit = "f7b96bb6bef5c5da4d3aa4f4e0498fbbf62dc78b";
constexpr const char *kFolder = "kokoro-en-1.0";
constexpr const char *kPartFolder = "kokoro-en-1.0.part";

// Measured from the files themselves, and checked against Hugging Face's own hashes. espeak-ng's are the few English
// needs, for words the word lists don't have, such as numbers and names.
const QList<NaturalVoiceFile> kFiles = {
    {"model.onnx", 325560556, "b40f62b166ac8164b0627ef48a0b358eda0985e272fb03ef5252e7206305da11"},
    {"voices.bin", 28200960, "1c5a5b983d3d50d8586d437a51f3faa2da7919ce76a013c081e65671a3447c29"},
    {"tokens.txt", 687, "6ebb6bb288f20f3ae8d004d3c2ca27697da27c037d75e81a60e2a6a663f95425"},
    {"lexicon-us-en.txt", 5956885, "7daaab53a181be9885b853a8582bf1838186317e5dadacbcef9c426d6fa0da14"},
    {"lexicon-gb-en.txt", 6366635, "c4cbb37316f62210dff52718a7afcaae24f50c032cc75ab47ae67b831d1049e7"},
    {"espeak-ng-data/phontab", 55796, "886f3fa402cb0ba73d483aa8ad000af47a6b7cc06293c75a97913fba68a530f6"},
    {"espeak-ng-data/phonindex", 39074, "3ca7b8fa3b42624e4b0f152707e7a39245fce569aa99ea47c055d9e622fcf0c4"},
    {"espeak-ng-data/phondata", 550424, "4e0288957874029a8c3c9f41a8f517ad4bf18127046decbdd4b9d1d6807ce3a3"},
    {"espeak-ng-data/intonations", 2040, "3f8af65fd3eda9759a10f021d61361c120871f463515229c925995c7f90918cc"},
    {"espeak-ng-data/en_dict", 166944, "71bd330ba8a2e3e8076e631508208ef49449d6147c17b7bd2b4b1e1468292e35"},
    {"espeak-ng-data/lang/gmw/en", 140, "4605d5330801de3641c6e366d15f129ea1f5ffbce8722642aba01ace07ab9c83"},
    {"espeak-ng-data/lang/gmw/en-US", 257, "41534c2a22df5dd4f1052ff9e1a33a3ea7bff5a26b5c02bdad5ba8ddb7524704"},
};

// The model's own numbering, from its list of speakers.
const QList<NaturalVoice> kVoices = {
    {"af_alloy", 0, "Alloy", false, false},     {"af_aoede", 1, "Aoede", false, false},
    {"af_bella", 2, "Bella", false, false},     {"af_heart", 3, "Heart", false, false},
    {"af_jessica", 4, "Jessica", false, false}, {"af_kore", 5, "Kore", false, false},
    {"af_nicole", 6, "Nicole", false, false},   {"af_nova", 7, "Nova", false, false},
    {"af_river", 8, "River", false, false},     {"af_sarah", 9, "Sarah", false, false},
    {"af_sky", 10, "Sky", false, false},        {"am_adam", 11, "Adam", false, true},
    {"am_echo", 12, "Echo", false, true},       {"am_eric", 13, "Eric", false, true},
    {"am_fenrir", 14, "Fenrir", false, true},   {"am_liam", 15, "Liam", false, true},
    {"am_michael", 16, "Michael", false, true}, {"am_onyx", 17, "Onyx", false, true},
    {"am_puck", 18, "Puck", false, true},       {"am_santa", 19, "Santa", false, true},
    {"bf_alice", 20, "Alice", true, false},     {"bf_emma", 21, "Emma", true, false},
    {"bf_isabella", 22, "Isabella", true, false}, {"bf_lily", 23, "Lily", true, false},
    {"bm_daniel", 24, "Daniel", true, true},    {"bm_fable", 25, "Fable", true, true},
    {"bm_george", 26, "George", true, true},    {"bm_lewis", 27, "Lewis", true, true},
};

void fetchFrom(Transport &transport, int index, const QString &part, const QString &folder, qint64 before,
               const std::shared_ptr<bool> &stop, const std::function<void(qint64)> &progress,
               const std::function<void(const QString &)> &done)
{
    const auto fail = [part, done](const QString &error) {
        QDir(part).removeRecursively();
        done(error);
    };
    if (*stop) {
        fail(kDownloadStopped);
        return;
    }
    if (index == kFiles.size()) {
        QDir(folder).removeRecursively();
        if (!QDir().rename(part, folder)) {
            fail(QStringLiteral("its folder couldn't be renamed"));
            return;
        }
        done(QString());
        return;
    }
    const NaturalVoiceFile &file = kFiles.at(index);
    const QString path = QDir(part).filePath(QString::fromLatin1(file.path));
    QDir().mkpath(QFileInfo(path).path());
    DownloadRequest request;
    request.url = naturalVoiceFileUrl(file);
    request.path = path;
    request.maxBytes = file.bytes;
    request.sha256 = QByteArray(file.sha256);
    request.redirectHosts = naturalVoiceHosts();
    request.stop = stop;
    if (progress)
        request.progress = [progress, before](qint64 received) { progress(before + received); };
    Transport *through = &transport;
    transport.download(request, [through, index, part, folder, before, stop, progress, done, fail, bytes = file.bytes,
                                 name = QString::fromLatin1(file.path)](const QString &error) {
        if (!error.isEmpty()) {
            fail(error == kDownloadStopped ? error : QStringLiteral("%1: %2").arg(name, error));
            return;
        }
        fetchFrom(*through, index + 1, part, folder, before + bytes, stop, progress, done);
    });
}

} // namespace

const QList<NaturalVoiceFile> &naturalVoiceFiles()
{
    return kFiles;
}

qint64 naturalVoicesBytes()
{
    qint64 total = 0;
    for (const NaturalVoiceFile &file : kFiles)
        total += file.bytes;
    return total;
}

const QList<NaturalVoice> &naturalVoices()
{
    return kVoices;
}

const NaturalVoice *naturalVoice(const QString &id)
{
    for (const NaturalVoice &voice : kVoices) {
        if (id == QLatin1StringView(voice.id))
            return &voice;
    }
    return nullptr;
}

const NaturalVoice &usualNaturalVoice(bool british)
{
    return *naturalVoice(british ? QStringLiteral("bf_emma") : QStringLiteral("af_heart"));
}

QUrl naturalVoiceFileUrl(const NaturalVoiceFile &file)
{
    return QUrl(QStringLiteral("https://huggingface.co/csukuangfj/kokoro-multi-lang-v1_0/resolve/%1/%2")
                    .arg(QLatin1StringView(kCommit), QLatin1StringView(file.path)));
}

QStringList naturalVoiceHosts()
{
    return {QStringLiteral("huggingface.co"), QStringLiteral(".hf.co")};
}

QString naturalVoicesFolder(const QString &voicesDirectory)
{
    return QDir(voicesDirectory).filePath(QLatin1StringView(kFolder));
}

bool naturalVoicesPresent(const QString &voicesDirectory)
{
    if (voicesDirectory.isEmpty())
        return false;
    const QDir folder(naturalVoicesFolder(voicesDirectory));
    for (const NaturalVoiceFile &file : kFiles) {
        const QFileInfo info(folder.filePath(QString::fromLatin1(file.path)));
        if (!info.isFile() || info.size() != file.bytes)
            return false;
    }
    return true;
}

bool removeNaturalVoices(const QString &voicesDirectory)
{
    if (voicesDirectory.isEmpty())
        return false;
    QDir(QDir(voicesDirectory).filePath(QLatin1StringView(kPartFolder))).removeRecursively();
    return QDir(naturalVoicesFolder(voicesDirectory)).removeRecursively();
}

std::shared_ptr<bool> downloadNaturalVoices(Transport &transport, const QString &voicesDirectory,
                                            const std::function<void(qint64 received)> &progress,
                                            const std::function<void(const QString &error)> &done)
{
    auto stop = std::make_shared<bool>(false);
    const QString part = QDir(voicesDirectory).filePath(QLatin1StringView(kPartFolder));
    QDir(part).removeRecursively();
    if (!QDir().mkpath(part)) {
        done(QStringLiteral("its folder couldn't be made"));
        return stop;
    }
    fetchFrom(transport, 0, part, naturalVoicesFolder(voicesDirectory), 0, stop, progress, done);
    return stop;
}

} // namespace papertrail::metadata
