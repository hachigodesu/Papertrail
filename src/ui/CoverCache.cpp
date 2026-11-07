#include "ui/CoverCache.h"

#include "core/model/Publication.h"
#include "import/ImportService.h"

#include <QGuiApplication>
#include <QImageReader>
#include <QMetaObject>

namespace papertrail::ui {

namespace {

constexpr int kCacheBytes = 96 * 1024 * 1024;

} // namespace

CoverCache::CoverCache(const QString &cacheDirectory, QObject *parent)
    : QObject(parent), m_cacheDirectory(cacheDirectory)
{
    m_pool.setMaxThreadCount(kMaxRunning);
    m_pixmaps.setMaxCost(kCacheBytes);
}

CoverCache::~CoverCache()
{
    m_waiting.clear();
    m_pool.waitForDone();
}

QPixmap CoverCache::cover(const QString &sha256)
{
    if (sha256.isEmpty())
        return {};
    const bool article = core::coverKeyPage(sha256) > 0;
    if (m_missing.contains(sha256))
        return article ? cover(core::coverKeyHash(sha256)) : QPixmap();
    if (const QPixmap *found = m_pixmaps.object(sha256))
        return *found;

    if (!m_running.contains(sha256)) {
        m_waiting.removeOne(sha256);
        m_waiting << sha256;
        if (m_waiting.size() > kMaxWaiting)
            m_waiting.removeFirst(); // Asked for again if it comes back into view.
        startNext();
    }
    // While an article's own cover loads, its file's is better than nothing.
    return article ? cover(core::coverKeyHash(sha256)) : QPixmap();
}

void CoverCache::coverMade(const QString &key)
{
    m_missing.remove(key);
    m_pixmaps.remove(key);
    cover(key);
}

void CoverCache::forgetMissing()
{
    m_missing.clear();
}

QImage CoverCache::fullCover(const QString &sha256) const
{
    if (sha256.isEmpty())
        return {};
    QImageReader reader(import::coverPathFor(m_cacheDirectory, sha256));
    const QImage image = reader.read();
    if (image.isNull() && core::coverKeyPage(sha256) > 0)
        return fullCover(core::coverKeyHash(sha256));
    return image;
}

void CoverCache::startNext()
{
    const QSize loadedSize = cardSize() * qApp->devicePixelRatio();
    while (m_running.size() < kMaxRunning && !m_waiting.isEmpty()) {
        const QString sha256 = m_waiting.takeLast();
        m_running.insert(sha256);
        const QString path = import::coverPathFor(m_cacheDirectory, sha256);
        // The destructor waits for these to finish, so `this` is still good here.
        m_pool.start([this, sha256, path, loadedSize] {
            QImageReader reader(path);
            const QImage full = reader.read();
            const QImage image = full.width() > loadedSize.width() || full.height() > loadedSize.height()
                ? full.scaled(loadedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : full;
            QMetaObject::invokeMethod(
                this, [this, sha256, image] { onLoaded(sha256, image); }, Qt::QueuedConnection);
        });
    }
}

void CoverCache::onLoaded(const QString &sha256, const QImage &image)
{
    m_running.remove(sha256);
    if (image.isNull()) {
        m_missing.insert(sha256);
        if (core::coverKeyPage(sha256) > 0 && !m_wanted.contains(sha256)) {
            m_wanted.insert(sha256);
            emit articleCoverWanted(sha256);
        }
    } else {
        auto *pixmap = new QPixmap(QPixmap::fromImage(image));
        const int cost = int(image.sizeInBytes());
        m_pixmaps.insert(sha256, pixmap, cost);
        emit coverReady(sha256);
    }
    startNext();
}

} // namespace papertrail::ui
