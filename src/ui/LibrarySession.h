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

struct LibraryPaths {
    QString database;
    QString cacheDirectory; // Covers go in a folder inside this one.
    QString workerPath;

    // Files changed more recently than this are left for a later scan. See ImportOptions.
    qint64 settleMs = 5000;

    // Where online sources are, and what reaches them. Only tests change these.
    metadata::ProviderUrls providerUrls;
    std::function<std::unique_ptr<metadata::Transport>()> transportFactory;

    // Where the Claude API key is kept. Only tests change this.
    std::function<std::unique_ptr<ApiKeyStore>()> keyStoreFactory;

    // The user's own library, using the standard folders for this platform.
    static LibraryPaths standard();
};

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
