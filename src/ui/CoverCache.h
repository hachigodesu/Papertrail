#pragma once

#include <QCache>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThreadPool>

namespace papertrail::ui {

// Loads cover images off the window's thread, as they're asked for. Whatever was asked for most
// recently loads first, so scrolling quickly past thousands of covers doesn't leave a backlog.
class CoverCache : public QObject {
    Q_OBJECT

public:
    explicit CoverCache(const QString &cacheDirectory, QObject *parent = nullptr);
    ~CoverCache() override;

    // Covers are held at the size the grid shows them, so that drawing one doesn't mean scaling it
    // on every frame while scrolling.
    static QSize cardSize() { return {150, 200}; }

    // Returns a null pixmap if it isn't loaded yet, or if there's no cover. `coverReady` follows
    // when one has loaded.
    QPixmap cover(const QString &sha256);

    // The cover at full size, read straight away, for showing one larger.
    QImage fullCover(const QString &sha256) const;

    // Covers found missing are checked for again, after an import has written more.
    void forgetMissing();

signals:
    void coverReady(const QString &sha256);

private:
    void startNext();
    void onLoaded(const QString &sha256, const QImage &image);

    static constexpr int kMaxRunning = 2;
    static constexpr int kMaxWaiting = 64;

    QString m_cacheDirectory;
    QThreadPool m_pool;
    QCache<QString, QPixmap> m_pixmaps;
    QStringList m_waiting;
    QSet<QString> m_running;
    QSet<QString> m_missing;
};

} // namespace papertrail::ui
