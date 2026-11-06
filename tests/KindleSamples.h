#pragma once

// Builds the awkward kinds of Kindle file from the committed samples, since there's no tool here to
// make them: a joint file, and one locked with DRM.

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtEndian>

namespace kindle_samples {

inline QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

inline bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline quint32 be32(const char *data)
{
    return qFromBigEndian<quint32>(data);
}

inline QByteArray toBe32(quint32 value)
{
    QByteArray bytes(4, 0);
    qToBigEndian(value, bytes.data());
    return bytes;
}

inline QByteArray toBe16(quint16 value)
{
    QByteArray bytes(2, 0);
    qToBigEndian(value, bytes.data());
    return bytes;
}

// The records of a Palm database file, which is what a Kindle file is.
inline QList<QByteArray> recordsOf(const QByteArray &file)
{
    const int count = qFromBigEndian<quint16>(file.constData() + 76);
    QList<QByteArray> records;
    for (int i = 0; i < count; ++i) {
        const quint32 start = be32(file.constData() + 78 + i * 8);
        const quint32 end = i + 1 < count ? be32(file.constData() + 78 + (i + 1) * 8) : quint32(file.size());
        records << file.mid(start, end - start);
    }
    return records;
}

inline QByteArray fileFrom(const QByteArray &header, const QList<QByteArray> &records)
{
    QByteArray out = header.left(76) + toBe16(quint16(records.size()));
    quint32 offset = quint32(78 + records.size() * 8 + 2);
    for (int i = 0; i < records.size(); ++i) {
        out += toBe32(offset) + toBe32(quint32(i * 2));
        offset += quint32(records.at(i).size());
    }
    out += QByteArray(2, 0);
    for (const QByteArray &record : records)
        out += record;
    return out;
}

// An old MOBI section, followed by a KF8 section, as Amazon's own tools make them. The first record's
// EXTH block says where the second section starts.
inline QByteArray jointFile(const QByteArray &oldMobi, const QByteArray &kf8)
{
    QList<QByteArray> oldRecords = recordsOf(oldMobi);
    const QList<QByteArray> kf8Records = recordsOf(kf8);

    QByteArray first = oldRecords.first();
    const quint32 headerLength = be32(first.constData() + 16 + 4);
    const qsizetype afterHeader = 16 + headerLength;
    const quint32 boundary = quint32(oldRecords.size() + 1);
    const QByteArray entry = toBe32(121) + toBe32(12) + toBe32(boundary);

    const quint32 flags = be32(first.constData() + 128);
    const bool hasExth = flags & 0x40;
    if (hasExth) {
        const quint32 total = be32(first.constData() + afterHeader + 4);
        const quint32 entries = be32(first.constData() + afterHeader + 8);
        first.replace(afterHeader + 4, 4, toBe32(total + 12));
        first.replace(afterHeader + 8, 4, toBe32(entries + 1));
        first.insert(afterHeader + 12, entry);
    } else {
        first.insert(afterHeader, QByteArray("EXTH") + toBe32(24) + toBe32(1) + entry);
        first.replace(128, 4, toBe32(flags | 0x40));
    }
    const qsizetype added = hasExth ? 12 : 24;
    first.replace(84, 4, toBe32(be32(first.constData() + 84) + quint32(added))); // The full name moved along.
    oldRecords[0] = first;

    QList<QByteArray> all = oldRecords;
    all << QByteArray("BOUNDARY");
    all += kf8Records;
    return fileFrom(oldMobi, all);
}

// The same book, marked as locked. Nothing's really encrypted, but this is what the header says.
inline QByteArray lockedFile(const QByteArray &file)
{
    QByteArray locked = file;
    const quint32 record0 = be32(file.constData() + 78);
    locked.replace(record0 + 12, 2, toBe16(2));
    return locked;
}

} // namespace kindle_samples
