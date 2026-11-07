#include "transfer/Zip.h"

#include <zlib.h>

namespace papertrail::transfer {

namespace {

constexpr quint32 kLocalHeader = 0x04034b50;
constexpr quint32 kCentralHeader = 0x02014b50;
constexpr quint32 kEndOfCentral = 0x06054b50;
constexpr quint16 kStored = 0;
constexpr quint16 kDeflated = 8;
constexpr quint16 kUtf8Names = 1 << 11;
// 1 January 2000, as a DOS date with no time, so the same contents always make the same archive.
constexpr quint16 kDosDate = ((2000 - 1980) << 9) | (1 << 5) | 1;

void put16(QByteArray &out, quint16 value)
{
    out.append(char(value & 0xFF));
    out.append(char((value >> 8) & 0xFF));
}

void put32(QByteArray &out, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.append(char((value >> shift) & 0xFF));
}

// Reading past either end gives 0, which every caller treats as a damaged archive.
quint16 get16(const QByteArray &data, qsizetype at)
{
    if (at < 0 || at + 2 > data.size())
        return 0;
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData()) + at;
    return quint16(bytes[0] | (bytes[1] << 8));
}

quint32 get32(const QByteArray &data, qsizetype at)
{
    if (at < 0 || at + 4 > data.size())
        return 0;
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData()) + at;
    return quint32(bytes[0]) | (quint32(bytes[1]) << 8) | (quint32(bytes[2]) << 16) | (quint32(bytes[3]) << 24);
}

quint32 crcOf(const QByteArray &data)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    qsizetype done = 0;
    while (done < data.size()) {
        const uInt chunk = uInt(qMin<qsizetype>(data.size() - done, 1 << 30));
        crc = crc32(crc, reinterpret_cast<const Bytef *>(data.constData() + done), chunk);
        done += chunk;
    }
    return quint32(crc);
}

// Raw deflate, as zip files use, without zlib's own header.
std::optional<QByteArray> deflateRaw(const QByteArray &data)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return std::nullopt;
    QByteArray out;
    out.resize(qsizetype(deflateBound(&stream, uLong(data.size()))) + 64);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const qsizetype written = qsizetype(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return std::nullopt;
    out.resize(written);
    return out;
}

std::optional<QByteArray> inflateRaw(const QByteArray &data, quint32 size)
{
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return std::nullopt;
    QByteArray out;
    out.resize(qsizetype(size));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    const int result = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || produced != size)
        return std::nullopt;
    return out;
}

} // namespace

void ZipWriter::add(const QString &name, const QByteArray &data)
{
    Entry entry;
    entry.name = name.toUtf8();
    entry.crc = crcOf(data);
    entry.size = quint32(data.size());
    entry.offset = quint32(m_out.size());

    QByteArray stored = data;
    entry.method = kStored;
    if (const std::optional<QByteArray> packed = deflateRaw(data); packed && packed->size() < data.size()) {
        stored = *packed;
        entry.method = kDeflated;
    }
    entry.compressedSize = quint32(stored.size());

    put32(m_out, kLocalHeader);
    put16(m_out, 20);         // Version needed.
    put16(m_out, kUtf8Names); // Flags.
    put16(m_out, entry.method);
    put16(m_out, 0);          // Time.
    put16(m_out, kDosDate);
    put32(m_out, entry.crc);
    put32(m_out, entry.compressedSize);
    put32(m_out, entry.size);
    put16(m_out, quint16(entry.name.size()));
    put16(m_out, 0); // Extra field.
    m_out += entry.name;
    m_out += stored;
    m_entries << entry;
}

QByteArray ZipWriter::finish()
{
    const quint32 centralStart = quint32(m_out.size());
    for (const Entry &entry : std::as_const(m_entries)) {
        put32(m_out, kCentralHeader);
        put16(m_out, 20); // Made by.
        put16(m_out, 20); // Version needed.
        put16(m_out, kUtf8Names);
        put16(m_out, entry.method);
        put16(m_out, 0);
        put16(m_out, kDosDate);
        put32(m_out, entry.crc);
        put32(m_out, entry.compressedSize);
        put32(m_out, entry.size);
        put16(m_out, quint16(entry.name.size()));
        put16(m_out, 0); // Extra field.
        put16(m_out, 0); // Comment.
        put16(m_out, 0); // Disk.
        put16(m_out, 0); // Internal attributes.
        put32(m_out, 0); // External attributes.
        put32(m_out, entry.offset);
        m_out += entry.name;
    }
    const quint32 centralSize = quint32(m_out.size()) - centralStart;
    put32(m_out, kEndOfCentral);
    put16(m_out, 0);
    put16(m_out, 0);
    put16(m_out, quint16(m_entries.size()));
    put16(m_out, quint16(m_entries.size()));
    put32(m_out, centralSize);
    put32(m_out, centralStart);
    put16(m_out, 0);
    QByteArray result = m_out;
    m_out.clear();
    m_entries.clear();
    return result;
}

bool ZipReader::open(const QByteArray &archive)
{
    m_archive = archive;
    m_entries.clear();
    m_error.clear();

    // The end record is in the last 64 KB, before any comment.
    qsizetype end = -1;
    const qsizetype earliest = qMax<qsizetype>(0, archive.size() - 22 - 65535);
    for (qsizetype at = archive.size() - 22; at >= earliest; --at) {
        if (get32(archive, at) == kEndOfCentral) {
            end = at;
            break;
        }
    }
    if (end < 0) {
        m_error = QStringLiteral("this isn't a zip file");
        return false;
    }
    const quint16 count = get16(archive, end + 10);
    const quint32 centralSize = get32(archive, end + 12);
    const quint32 centralStart = get32(archive, end + 16);
    if (get16(archive, end + 4) != 0 || get16(archive, end + 6) != 0 || centralStart == 0xFFFFFFFF
        || qint64(centralStart) + centralSize > end) {
        m_error = QStringLiteral("it's split into parts or uses a zip format this can't read");
        return false;
    }

    qsizetype at = centralStart;
    for (int i = 0; i < count; ++i) {
        if (at + 46 > end || get32(archive, at) != kCentralHeader) {
            m_error = QStringLiteral("its list of contents is damaged");
            return false;
        }
        const quint16 flags = get16(archive, at + 8);
        Entry entry;
        entry.method = get16(archive, at + 10);
        entry.crc = get32(archive, at + 16);
        entry.compressedSize = get32(archive, at + 20);
        entry.size = get32(archive, at + 24);
        const quint16 nameLength = get16(archive, at + 28);
        const quint16 extraLength = get16(archive, at + 30);
        const quint16 commentLength = get16(archive, at + 32);
        entry.localOffset = get32(archive, at + 42);
        if (at + 46 + nameLength > end) {
            m_error = QStringLiteral("its list of contents is damaged");
            return false;
        }
        const QByteArray name = archive.mid(at + 46, nameLength);
        entry.name = (flags & kUtf8Names) ? QString::fromUtf8(name) : QString::fromLatin1(name);
        if (flags & 1) {
            m_error = QStringLiteral("it's encrypted");
            return false;
        }
        m_entries << entry;
        at += 46 + nameLength + extraLength + commentLength;
    }
    return true;
}

QList<QString> ZipReader::names() const
{
    QList<QString> names;
    for (const Entry &entry : m_entries)
        names << entry.name;
    return names;
}

bool ZipReader::contains(const QString &name) const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(), [&](const Entry &entry) { return entry.name == name; });
}

std::optional<QByteArray> ZipReader::read(const QString &name, qint64 maxSize)
{
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.name != name)
            continue;
        if (qint64(entry.size) > maxSize) {
            m_error = QStringLiteral("%1 is too large").arg(name);
            return std::nullopt;
        }
        const qsizetype at = entry.localOffset;
        if (at + 30 > m_archive.size() || get32(m_archive, at) != kLocalHeader) {
            m_error = QStringLiteral("%1 is damaged").arg(name);
            return std::nullopt;
        }
        const qsizetype dataStart = at + 30 + get16(m_archive, at + 26) + get16(m_archive, at + 28);
        if (dataStart + qsizetype(entry.compressedSize) > m_archive.size()) {
            m_error = QStringLiteral("%1 is cut short").arg(name);
            return std::nullopt;
        }
        const QByteArray stored = m_archive.mid(dataStart, entry.compressedSize);
        std::optional<QByteArray> data;
        if (entry.method == kStored)
            data = stored.size() == qsizetype(entry.size) ? std::optional<QByteArray>(stored) : std::nullopt;
        else if (entry.method == kDeflated)
            data = inflateRaw(stored, entry.size);
        else {
            m_error = QStringLiteral("%1 is compressed in a way this can't read").arg(name);
            return std::nullopt;
        }
        if (!data || crcOf(*data) != entry.crc) {
            m_error = QStringLiteral("%1 is damaged").arg(name);
            return std::nullopt;
        }
        return data;
    }
    m_error = QStringLiteral("%1 isn't in it").arg(name);
    return std::nullopt;
}

} // namespace papertrail::transfer
