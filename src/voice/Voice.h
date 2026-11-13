#pragma once

#include <QString>

#include <memory>
#include <vector>

// Kokoro's natural voices, through sherpa-onnx. This is the only part of Papertrail that calls sherpa-onnx, and only the
// worker links it, since what it reads comes from documents and espeak-ng inside it has had memory bugs.
namespace papertrail::voice {

// sherpa-onnx's version, such as "1.13.8".
QString version();

class Voice {
public:
    Voice();
    ~Voice();
    Voice(const Voice &) = delete;
    Voice &operator=(const Voice &) = delete;

    // Loads the model from the voices' folder, with British or American English's word list. Empty if it's ready, or
    // what went wrong. Takes a few seconds, and about 600 MB while loaded.
    QString load(const QString &folder, bool british, int threads);
    bool isLoaded() const;
    QString folder() const { return m_folder; }
    bool british() const { return m_british; }

    // One sentence, as 16-bit samples at sampleRate(), by the model's speaker number. Faster than 1 is quicker.
    // Empty, with `error` set, if it couldn't be said.
    std::vector<qint16> say(const QString &text, int speaker, double speed, QString *error);
    int sampleRate() const;

private:
    struct Engine;
    std::unique_ptr<Engine> m_engine;
    QString m_folder;
    bool m_british = false;
};

} // namespace papertrail::voice
