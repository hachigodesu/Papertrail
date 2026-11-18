#pragma once

#include "citation/CitationItem.h"
#include "citation/CslStyles.h"
#include "citation/Formatting.h"
#include "citation/Styles.h"

#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

class QThread;

namespace papertrail::citation {

class CslEngine;

// Renders CSL styles on a thread of its own, with an engine kept for the last style, so the window never waits for
// citeproc-js. Each request names a channel, such as "copy" or "preview": a newer one on the same channel replaces
// one still waiting and stops one being rendered, so only the newest preview is worked on.
class CslThread : public QObject {
    Q_OBJECT

public:
    enum class Kind {
        References, // A reference list, in the style's order.
        Citation,   // One in-text citation, or note, for all the items.
    };

    explicit CslThread(QObject *parent = nullptr);
    ~CslThread() override;

    // Returns the request's number, which `finished` gives back.
    qint64 render(const QString &channel, Kind kind, const CslLayout &layout, const QList<CitationItem> &items,
                  const StyleOptions &options = {});
    // Stops whatever is waiting or being rendered on this channel, without an answer.
    void cancel(const QString &channel);

    // For tests: how long a style may take (see CslEngine::setLimits).
    void setLimits(std::chrono::milliseconds base, std::chrono::milliseconds perItem);

signals:
    // For References, `entries` is the list and `order` says which item each is; for Citation, it's one entry.
    // Empty `error` means it worked.
    void finished(qint64 request, const QList<papertrail::citation::RichText> &entries, const QList<int> &order,
                  const QString &error);

private:
    struct Request {
        qint64 id = 0;
        QString channel;
        Kind kind = Kind::References;
        CslLayout layout;
        QList<CitationItem> items;
        StyleOptions options;
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Request> m_waiting;
    std::shared_ptr<Request> m_current; // What's being rendered, so it can be stopped.
    bool m_stopping = false;
    qint64 m_next = 0;
    std::chrono::milliseconds m_base{60000};
    std::chrono::milliseconds m_perItem{100};
    bool m_limitsChanged = false;
    QThread *m_thread = nullptr;
};

} // namespace papertrail::citation
