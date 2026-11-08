#include "core/db/Database.h"

#include <sqlite3.h>

namespace papertrail::core {

namespace {

[[noreturn]] void fail(sqlite3 *db, int code, const std::string &context)
{
    std::string message = context + ": ";
    message += db ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    throw DbError(code, message);
}

} // namespace

Statement::Statement(sqlite3 *db, const char *sql)
    : m_db(db)
{
    const int rc = sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr);
    if (rc != SQLITE_OK)
        fail(db, rc, std::string("prepare failed for [") + sql + "]");
}

Statement::~Statement()
{
    sqlite3_finalize(m_stmt);
}

Statement &Statement::bind(int index, qint64 value)
{
    const int rc = sqlite3_bind_int64(m_stmt, index, value);
    if (rc != SQLITE_OK)
        fail(m_db, rc, "bind failed");
    return *this;
}

Statement &Statement::bind(int index, int value)
{
    return bind(index, qint64(value));
}

Statement &Statement::bind(int index, double value)
{
    const int rc = sqlite3_bind_double(m_stmt, index, value);
    if (rc != SQLITE_OK)
        fail(m_db, rc, "bind failed");
    return *this;
}

Statement &Statement::bind(int index, const QString &value)
{
    if (value.isNull())
        return bindNull(index);
    const QByteArray utf8 = value.toUtf8();
    const int rc = sqlite3_bind_text(m_stmt, index, utf8.constData(), int(utf8.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
        fail(m_db, rc, "bind failed");
    return *this;
}

Statement &Statement::bind(int index, const char *value)
{
    return bind(index, QString::fromUtf8(value));
}

Statement &Statement::bindNull(int index)
{
    const int rc = sqlite3_bind_null(m_stmt, index);
    if (rc != SQLITE_OK)
        fail(m_db, rc, "bind failed");
    return *this;
}

bool Statement::step()
{
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    // Reset straight away, so a failed statement can't hold a transaction open.
    std::string message = sqlite3_errmsg(m_db);
    sqlite3_reset(m_stmt);
    throw DbError(rc, "step failed: " + message);
}

void Statement::reset()
{
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

int Statement::columnCount() const
{
    return sqlite3_column_count(m_stmt);
}

bool Statement::isNull(int column) const
{
    return sqlite3_column_type(m_stmt, column) == SQLITE_NULL;
}

qint64 Statement::int64At(int column) const
{
    return sqlite3_column_int64(m_stmt, column);
}

int Statement::intAt(int column) const
{
    return sqlite3_column_int(m_stmt, column);
}

double Statement::doubleAt(int column) const
{
    return sqlite3_column_double(m_stmt, column);
}

QString Statement::textAt(int column) const
{
    if (isNull(column))
        return {};
    const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, column));
    const int size = sqlite3_column_bytes(m_stmt, column);
    return QString::fromUtf8(text, size);
}

std::optional<int> Statement::optIntAt(int column) const
{
    if (isNull(column))
        return std::nullopt;
    return intAt(column);
}

std::optional<qint64> Statement::optInt64At(int column) const
{
    if (isNull(column))
        return std::nullopt;
    return int64At(column);
}

Database::Database(const QString &path)
{
    const QByteArray utf8 = path.toUtf8();
    const int rc = sqlite3_open_v2(utf8.constData(), &m_db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = "cannot open database";
        if (m_db)
            message += std::string(": ") + sqlite3_errmsg(m_db);
        sqlite3_close(m_db);
        m_db = nullptr;
        throw DbError(rc, message);
    }

    sqlite3_busy_timeout(m_db, 5000);
    try {
        exec("PRAGMA foreign_keys = ON");
        exec("PRAGMA journal_mode = WAL");
        exec("PRAGMA synchronous = NORMAL");
        exec("PRAGMA temp_store = MEMORY");
        exec("PRAGMA cache_size = -64000"); // 64 MB
        exec("PRAGMA mmap_size = 268435456"); // 256 MB
    } catch (...) {
        sqlite3_close(m_db);
        m_db = nullptr;
        throw;
    }
}

Database::~Database()
{
    m_cache.clear(); // Statements must be finalised before the connection closes.
    sqlite3_close(m_db);
}

void Database::exec(const char *sql)
{
    char *error = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errstr(rc);
        sqlite3_free(error);
        throw DbError(rc, "exec failed: " + message);
    }
}

std::unique_ptr<Statement> Database::prepare(const char *sql)
{
    return std::make_unique<Statement>(m_db, sql);
}

Statement &Database::cached(const char *sql)
{
    auto &slot = m_cache[sql];
    if (slot)
        slot->reset();
    else
        slot = std::make_unique<Statement>(m_db, sql);
    return *slot;
}

qint64 Database::lastInsertId() const
{
    return sqlite3_last_insert_rowid(m_db);
}

int Database::changes() const
{
    return sqlite3_changes(m_db);
}

QByteArray Database::changeStamp()
{
    // data_version covers other connections, and total_changes covers this one.
    Statement &version = cached("PRAGMA data_version");
    version.step();
    const qint64 dataVersion = version.int64At(0);
    version.reset();
    return QByteArray::number(dataVersion) + '/' + QByteArray::number(sqlite3_total_changes64(m_db)) + '/'
        + QByteArray::number(m_replaced);
}

void Database::replaceWith(const QString &path)
{
    if (inTransaction())
        throw DbError(SQLITE_MISUSE, "can't replace a database during a transaction");
    m_cache.clear(); // Nothing of this connection's may be reading while its pages change.
    sqlite3 *source = nullptr;
    const QByteArray utf8 = path.toUtf8();
    int rc = sqlite3_open_v2(utf8.constData(), &source, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = std::string("cannot open the copy: ") + (source ? sqlite3_errmsg(source) : sqlite3_errstr(rc));
        sqlite3_close(source);
        throw DbError(rc, message);
    }
    sqlite3_backup *backup = sqlite3_backup_init(m_db, "main", source, "main");
    if (!backup) {
        const std::string message = std::string("cannot start copying: ") + sqlite3_errmsg(m_db);
        sqlite3_close(source);
        throw DbError(sqlite3_errcode(m_db), message);
    }
    // In one step, so other connections never see half of it. They're waited for as any write would.
    for (int attempt = 0; attempt < 50; ++attempt) {
        rc = sqlite3_backup_step(backup, -1);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
            break;
        sqlite3_sleep(100);
    }
    sqlite3_backup_finish(backup);
    sqlite3_close(source);
    if (rc != SQLITE_DONE)
        throw DbError(rc, std::string("cannot copy the database: ") + sqlite3_errstr(rc));
    ++m_replaced;
}

bool Database::inTransaction() const
{
    return sqlite3_get_autocommit(m_db) == 0;
}

int Database::userVersion()
{
    auto statement = prepare("PRAGMA user_version");
    statement->step();
    return statement->intAt(0);
}

void Database::setUserVersion(int version)
{
    // PRAGMA arguments can't be bound, but this is an integer rather than outside input.
    exec(("PRAGMA user_version = " + std::to_string(version)).c_str());
}

Transaction::Transaction(Database &db)
    : m_db(db), m_nested(db.inTransaction())
{
    m_db.exec(m_nested ? "SAVEPOINT papertrail_nested" : "BEGIN IMMEDIATE");
}

Transaction::~Transaction()
{
    if (m_done)
        return;
    try {
        if (m_nested) {
            m_db.exec("ROLLBACK TO papertrail_nested");
            m_db.exec("RELEASE papertrail_nested");
        } else {
            m_db.exec("ROLLBACK");
        }
    } catch (const DbError &) {
        // A destructor can't throw. SQLite has already rolled back in the cases that land here.
    }
}

void Transaction::commit()
{
    m_db.exec(m_nested ? "RELEASE papertrail_nested" : "COMMIT");
    m_done = true;
}

} // namespace papertrail::core
