#pragma once

#include "metadata/Transport.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

// Kokoro's natural voices for reading aloud, which don't come with Papertrail since they're 367 MB. They're fetched
// when the user asks, from the copy sherpa-onnx publishes on Hugging Face at a fixed commit, and only kept if every file
// is exactly the one expected. Only English's are offered.
namespace papertrail::metadata {

struct NaturalVoiceFile {
    const char *path; // Inside the voices' folder, with forward slashes.
    qint64 bytes;
    const char *sha256;
};

struct NaturalVoice {
    const char *id;  // Kokoro's, such as "bf_emma".
    int speaker;     // Its number in the model.
    const char *name; // Such as "Emma".
    bool british;
    bool male;
};

// Every file needed, and how much they come to.
const QList<NaturalVoiceFile> &naturalVoiceFiles();
qint64 naturalVoicesBytes();

// Every voice offered, American first, then British.
const QList<NaturalVoice> &naturalVoices();
const NaturalVoice *naturalVoice(const QString &id);
// The one used when none is chosen: Emma for a British locale, else Heart.
const NaturalVoice &usualNaturalVoice(bool british);

// Where a file is. Only ever this address, built here.
QUrl naturalVoiceFileUrl(const NaturalVoiceFile &file);
// Where Hugging Face may send a request on to: its own site, and the network it serves large files from.
QStringList naturalVoiceHosts();

// The folder the voices are kept in, inside `voicesDirectory`, and whether every file is there at its size.
QString naturalVoicesFolder(const QString &voicesDirectory);
bool naturalVoicesPresent(const QString &voicesDirectory);
// Deletes the voices' folder, and any fetch left half done. Nothing else in `voicesDirectory` is touched.
bool removeNaturalVoices(const QString &voicesDirectory);

// Fetches every file, one at a time, into a folder of its own that's renamed once all are there, so the voices are
// either all there or not at all. `progress` is told how much has arrived of `naturalVoicesBytes()`, and `done` an empty
// string once they're ready, or what went wrong, with nothing kept. Setting the flag returned stops it. Only ever
// called when the user asks, and only while lookups are allowed online, which the caller checks.
std::shared_ptr<bool> downloadNaturalVoices(Transport &transport, const QString &voicesDirectory,
                                            const std::function<void(qint64 received)> &progress,
                                            const std::function<void(const QString &error)> &done);

} // namespace papertrail::metadata
