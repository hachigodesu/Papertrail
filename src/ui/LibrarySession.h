#pragma once

#include "core/db/Database.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/TagRepository.h"
#include "core/settings/SettingsStore.h"
#include "metadata/Providers.h"
#include "metadata/Transport.h"

#include <QLockFile>
#include <QString>

#include <functional>
#include <functional>
#include <memory>

namespace papertrail::ui {

class ApiKeyStore;
class Speaker;

struct LibraryPaths {
    QString database;
    QString cacheDirectory; // Covers go in a folder inside this one.
    QString workerPath;
    // The models for recognising text, one file a language. English comes with Papertrail, and is copied here.
    QString tessdataDirectory;

    // Files changed more recently than this are left for a later scan. See ImportOptions.
    qint64 settleMs = 5000;

    // How long after the window opens the day's backup is made, so it doesn't compete with opening.
    int dailyBackupDelayMs = 10000;

    // Time between requests checking for retractions. Only tests change this.
    int noticeCheckIntervalMs = -1;

    // Where online sources are, and what reaches them. Only tests change these.
    metadata::ProviderUrls providerUrls;
    std::function<std::unique_ptr<metadata::Transport>()> transportFactory;

    // What reads aloud. Only tests change this, with one that only pretends.
    std::function<std::unique_ptr<Speaker>()> speakerFactory;

    // Where keys of the user's own are kept, by their entry's name, such as kClaudeKeyEntry. Only tests change this.
    std::function<std::unique_ptr<ApiKeyStore>(const QString &entry)> keyStoreFactory;

    // The user's own library, using the standard folders for this platform.
    static LibraryPaths standard();
    // A library kept in one folder, with its cache inside it.
    static LibraryPaths inFolder(const QString &folder);
    // A portable copy, with a file named portable.txt beside the program, keeps its library in a folder beside it,
    // so it can be carried on a USB stick. Any other copy uses the standard folders.
    static LibraryPaths forThisCopy();
    static constexpr const char *kPortableMarker = "portable.txt";
    static constexpr const char *kPortableFolder = "Papertrail library";
};

// Whether this copy of Papertrail can recognise text in scanned pages.
bool canRecogniseText();

// Where the language models that come with Papertrail are: beside the program, in its shared data, or, for a build
// that hasn't been installed, in the source. Empty if there are none.
QString bundledTessdata();

// Copies the models that come with Papertrail into `tessdata`, where any downloaded ones go too, since Tesseract reads
// every language from one folder. Leaves any there already.
void prepareTessdata(const QString &tessdata);

// Thrown when another copy of Papertrail already has this library open.
class LibraryInUse : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The open library, as used by the window's own thread. Background work opens its own connections.
class LibrarySession {
public:
    // Creates the library if needed and brings it up to date. Throws LibraryInUse, or DbError if
    // the database can't be used.
    explicit LibrarySession(const LibraryPaths &paths);

    const LibraryPaths &paths() const { return m_paths; }

    core::Database &database() { return *m_db; }
    core::PublicationRepository &publications() { return *m_publications; }
    core::TagRepository &tags() { return *m_tags; }
    core::CollectionRepository &collections() { return *m_collections; }
    core::FileRepository &files() { return *m_files; }
    core::SettingsStore &settings() { return *m_settings; }
    core::LibraryQuery &query() { return *m_query; }

private:
    LibraryPaths m_paths;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<core::Database> m_db;
    std::unique_ptr<core::PublicationRepository> m_publications;
    std::unique_ptr<core::TagRepository> m_tags;
    std::unique_ptr<core::CollectionRepository> m_collections;
    std::unique_ptr<core::FileRepository> m_files;
    std::unique_ptr<core::SettingsStore> m_settings;
    std::unique_ptr<core::LibraryQuery> m_query;
};

} // namespace papertrail::ui
