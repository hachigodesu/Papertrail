#pragma once

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

namespace papertrail::core {

class DbError : public std::runtime_error {
public:
    DbError(int code, const std::string &message)
        : std::runtime_error(message), m_code(code) {}
    int code() const { return m_code; }

private:
    int m_code;
};

// A prepared SQL statement. Bind indexes are 1-based and column indexes 0-based, as in SQLite.
class Statement {
public:
    Statement(sqlite3 *db, const char *sql);
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    Statement &bind(int index, qint64 value);
    Statement &bind(int index, int value);
    Statement &bind(int index, double value);
    Statement &bind(int index, const QString &value); // A null QString binds NULL.
    Statement &bind(int index, const char *value);
    Statement &bindNull(int index);

    template <typename T>
    Statement &bind(int index, const std::optional<T> &value)
    {
        return value ? bind(index, *value) : bindNull(index);
    }

    // Returns true if a row is available, false when the statement has finished.
    bool step();
    void reset();

    int columnCount() const;
    bool isNull(int column) const;
    qint64 int64At(int column) const;
    int intAt(int column) const;
    double doubleAt(int column) const;
    QString textAt(int column) const; // NULL becomes a null QString.
    std::optional<int> optIntAt(int column) const;
    std::optional<qint64> optInt64At(int column) const;

private:
    sqlite3 *m_db;
    sqlite3_stmt *m_stmt = nullptr;
};

// One connection to a SQLite database file. Use one per thread.
class Database {
public:
    // Opens or creates the file, with foreign keys on and WAL journalling.
    explicit Database(const QString &path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    void exec(const char *sql);
    std::unique_ptr<Statement> prepare(const char *sql);
    std::unique_ptr<Statement> prepare(const QByteArray &sql) { return prepare(sql.constData()); }

    // A reusable statement, reset and ready to bind. `sql` must be a string literal or
    // otherwise fixed text. Don't reuse a statement while an earlier use is still being read.
    Statement &cached(const char *sql);

    qint64 lastInsertId() const;
    int changes() const;

    // A value that changes whenever data has changed, through this connection or another.
    // Compare two values to tell whether something cached is still good.
    QByteArray changeStamp();

    bool inTransaction() const;
    int userVersion();
    void setUserVersion(int version);

    sqlite3 *handle() const { return m_db; }

private:
    sqlite3 *m_db = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Statement>> m_cache;
};

// Commits only when commit() is called, and otherwise rolls back when it goes out of scope.
// Nests safely: an inner transaction becomes a savepoint.
class Transaction {
public:
    explicit Transaction(Database &db);
    ~Transaction();
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit();

private:
    Database &m_db;
    bool m_nested;
    bool m_done = false;
};

} // namespace papertrail::core
