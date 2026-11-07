#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::transfer {

// A small zip writer and reader, enough for a library export: a handful of files, each compressed with
// deflate. Names are UTF-8. There's no support for encryption, zip64 or spanning several files, and a
// reader refuses anything that would need them rather than guessing.
class ZipWriter {
public:
    void add(const QString &name, const QByteArray &data);
    QByteArray finish();

private:
    struct Entry {
        QByteArray name;
        quint32 crc = 0;
        quint32 compressedSize = 0;
        quint32 size = 0;
        quint32 offset = 0;
        quint16 method = 0;
    };
    QByteArray m_out;
    QList<Entry> m_entries;
};

class ZipReader {
public:
    // Reads the archive's list of files. Returns false, with `error()` saying why, if it isn't one this can read.
    bool open(const QByteArray &archive);
    QString error() const { return m_error; }

    QList<QString> names() const;
    bool contains(const QString &name) const;

    // A file's contents, checked against its stored checksum. Empty with `error()` set if it couldn't be read.
    // Nothing larger than `maxSize` is unpacked, so a crafted archive can't use up all memory.
    std::optional<QByteArray> read(const QString &name, qint64 maxSize = 512LL * 1024 * 1024);

private:
    struct Entry {
        QString name;
        quint16 method = 0;
        quint32 crc = 0;
        quint32 compressedSize = 0;
        quint32 size = 0;
        quint32 localOffset = 0;
    };
    QByteArray m_archive;
    QList<Entry> m_entries;
    QString m_error;
};

} // namespace papertrail::transfer
