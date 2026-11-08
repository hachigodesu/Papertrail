#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// Copies of the library database, kept in a "backups" folder beside it. Each is a complete database made by
// SQLite's VACUUM INTO, so it can be opened on its own, and is named for why and when it was made.
enum class BackupKind {
    Daily,          // At most one a day, when the library is open. The newest few are kept.
    BeforeUpgrade,  // Before a newer Papertrail changes the database's layout.
    BeforeImport,   // Before importing an export replaces the library.
    BeforeRestore,  // Before a backup replaces the library, so restoring can be undone.
    ByHand,         // Asked for. Never removed automatically.
};

struct BackupInfo {
    QString path;
    BackupKind kind = BackupKind::Daily;
    QDateTime made;
    qint64 size = 0;
};

// How many of each kind are kept: daily ones as the user chose, and a few of the others made automatically.
constexpr int kDailyBackupsKept = 7;
constexpr int kOtherBackupsKept = 3;

QString backupFolder(const QString &databasePath);

// A path in the backup folder for a new backup of this kind, not yet used by any file.
QString newBackupPath(const QString &databasePath, BackupKind kind, const QDateTime &when = QDateTime::currentDateTime());

// Makes a complete copy of the library's own database (not anything attached to it) and returns its path.
// Throws DbError if it can't, such as when the disk is full, and leaves no partial file behind.
QString makeBackup(Database &db, const QString &databasePath, BackupKind kind,
                   const QDateTime &when = QDateTime::currentDateTime());

// The backups there are, newest first. Files that don't look like Papertrail's own are left out.
QList<BackupInfo> backups(const QString &databasePath);

// Whether no daily backup has been made on `today` yet.
bool dailyBackupDue(const QString &databasePath, const QDate &today);

// Removes daily backups beyond the newest `keepDaily`, and of each other automatic kind beyond the newest
// `keepOthers`. Backups made by hand are never removed.
void pruneBackups(const QString &databasePath, int keepDaily = kDailyBackupsKept, int keepOthers = kOtherBackupsKept);

// Backs the library up if it's about to be upgraded to a newer layout, and returns the backup's path. Nothing
// happens for a new library, or one that's already up to date.
std::optional<QString> backUpBeforeUpgrade(Database &db, const QString &databasePath);

struct BackupCheck {
    bool usable = false;
    int schemaVersion = 0;
    QString problem; // Why it can't be used, for showing.
};

// Opens a backup without changing it, to see whether it's a Papertrail library this version can use.
BackupCheck checkBackup(const QString &backupPath);

// Replaces the open library's contents with the backup's, after keeping a copy of the library as it was, whose
// path is returned. The backup is brought up to date afterwards if it's from an older version. Throws DbError
// if the backup can't be used, in which case the library is unchanged.
QString restoreBackup(Database &db, const QString &databasePath, const QString &backupPath);

} // namespace papertrail::core
