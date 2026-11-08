#include "core/db/Backups.h"

#include "core/db/Database.h"
#include "core/db/Schema.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <sqlite3.h>

#include <algorithm>

namespace papertrail::core {

namespace {

const QString kStampFormat = QStringLiteral("yyyy-MM-dd-HHmmss");

QString kindName(BackupKind kind)
{
    switch (kind) {
    case BackupKind::Daily:
        return QStringLiteral("daily");
    case BackupKind::BeforeUpgrade:
        return QStringLiteral("before-upgrade");
    case BackupKind::BeforeImport:
        return QStringLiteral("before-import");
    case BackupKind::BeforeRestore:
        return QStringLiteral("before-restore");
    case BackupKind::ByHand:
        return QStringLiteral("by-hand");
    }
    return QStringLiteral("daily");
}

std::optional<BackupKind> kindFrom(const QString &name)
{
    for (const BackupKind kind : {BackupKind::Daily, BackupKind::BeforeUpgrade, BackupKind::BeforeImport,
                                  BackupKind::BeforeRestore, BackupKind::ByHand}) {
        if (kindName(kind) == name)
            return kind;
    }
    return std::nullopt;
}

int newestSchemaVersion()
{
    const auto &migrations = libraryMigrations();
    return migrations.empty() ? 0 : migrations.back().version;
}

} // namespace

QString backupFolder(const QString &databasePath)
{
    return QFileInfo(databasePath).absoluteDir().filePath(QStringLiteral("backups"));
}

QString newBackupPath(const QString &databasePath, BackupKind kind, const QDateTime &when)
{
    const QDir folder(backupFolder(databasePath));
    const QString base = QStringLiteral("library-%1-%2").arg(kindName(kind), when.toString(kStampFormat));
    QString path = folder.filePath(base + QStringLiteral(".db"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = folder.filePath(QStringLiteral("%1-%2.db").arg(base).arg(n));
    return path;
}

QString makeBackup(Database &db, const QString &databasePath, BackupKind kind, const QDateTime &when)
{
    QDir().mkpath(backupFolder(databasePath));
    const QString path = newBackupPath(databasePath, kind, when);
    try {
        auto copy = db.prepare("VACUUM main INTO ?");
        copy->bind(1, path);
        copy->step();
    } catch (...) {
        QFile::remove(path); // A partial copy would look like a real one.
        throw;
    }
    return path;
}

QList<BackupInfo> backups(const QString &databasePath)
{
    static const QRegularExpression pattern(
        QStringLiteral("^library-([a-z-]+)-(\\d{4}-\\d{2}-\\d{2}-\\d{6})(?:-\\d+)?\\.db$"));
    QList<BackupInfo> found;
    QHash<QString, QDateTime> written;
    const QDir folder(backupFolder(databasePath));
    for (const QFileInfo &file : folder.entryInfoList({QStringLiteral("library-*.db")}, QDir::Files)) {
        const QRegularExpressionMatch match = pattern.match(file.fileName());
        if (!match.hasMatch())
            continue;
        const std::optional<BackupKind> kind = kindFrom(match.captured(1));
        const QDateTime made = QDateTime::fromString(match.captured(2), kStampFormat);
        if (!kind || !made.isValid())
            continue;
        found.append({file.absoluteFilePath(), *kind, made, file.size()});
        written.insert(file.absoluteFilePath(), file.lastModified());
    }
    // Two made in the same second are told apart by when their files were written.
    std::sort(found.begin(), found.end(), [&written](const BackupInfo &a, const BackupInfo &b) {
        if (a.made != b.made)
            return a.made > b.made;
        const QDateTime first = written.value(a.path);
        const QDateTime second = written.value(b.path);
        return first != second ? first > second : a.path > b.path;
    });
    return found;
}

bool dailyBackupDue(const QString &databasePath, const QDate &today)
{
    for (const BackupInfo &backup : backups(databasePath)) {
        if (backup.kind == BackupKind::Daily && backup.made.date() == today)
            return false;
    }
    return true;
}

void pruneBackups(const QString &databasePath, int keepDaily, int keepOthers)
{
    QHash<int, int> seen;
    for (const BackupInfo &backup : backups(databasePath)) { // Newest first.
        if (backup.kind == BackupKind::ByHand)
            continue;
        const int kept = backup.kind == BackupKind::Daily ? keepDaily : keepOthers;
        if (++seen[int(backup.kind)] > kept)
            QFile::remove(backup.path);
    }
}

std::optional<QString> backUpBeforeUpgrade(Database &db, const QString &databasePath)
{
    const int version = db.userVersion();
    if (version == 0 || version >= newestSchemaVersion())
        return std::nullopt; // New, already up to date, or too new, which migrating refuses.
    const QString path = makeBackup(db, databasePath, BackupKind::BeforeUpgrade);
    pruneBackups(databasePath);
    return path;
}

BackupCheck checkBackup(const QString &backupPath)
{
    BackupCheck check;
    if (!QFileInfo(backupPath).isFile()) {
        check.problem = QStringLiteral("The file isn't there.");
        return check;
    }
    sqlite3 *db = nullptr;
    const QByteArray utf8 = backupPath.toUtf8();
    if (sqlite3_open_v2(utf8.constData(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        check.problem = QStringLiteral("It can't be opened as a database.");
        return check;
    }
    const auto single = [db](const char *sql) -> std::optional<QString> {
        sqlite3_stmt *statement = nullptr;
        std::optional<QString> value;
        if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW)
            value = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
        sqlite3_finalize(statement);
        return value;
    };
    const std::optional<QString> ok = single("PRAGMA quick_check");
    const std::optional<QString> version = single("PRAGMA user_version");
    const std::optional<QString> library = single("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = 'publication'");
    sqlite3_close(db);
    if (!ok || *ok != QLatin1StringView("ok")) {
        check.problem = QStringLiteral("It's damaged, or isn't a database.");
        return check;
    }
    check.schemaVersion = version ? version->toInt() : 0;
    if (!library || library->toInt() == 0 || check.schemaVersion == 0) {
        check.problem = QStringLiteral("It isn't a Papertrail library.");
        return check;
    }
    if (check.schemaVersion > newestSchemaVersion()) {
        check.problem = QStringLiteral("It was made by a newer version of Papertrail.");
        return check;
    }
    check.usable = true;
    return check;
}

QString restoreBackup(Database &db, const QString &databasePath, const QString &backupPath)
{
    const BackupCheck check = checkBackup(backupPath);
    if (!check.usable)
        throw DbError(SQLITE_ERROR, check.problem.toStdString());
    // The library as it is, so that restoring can itself be undone.
    const QString before = makeBackup(db, databasePath, BackupKind::BeforeRestore);
    db.replaceWith(backupPath);
    migrateLibrary(db);
    pruneBackups(databasePath);
    return before;
}

} // namespace papertrail::core
