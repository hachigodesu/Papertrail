#pragma once

#include "core/model/Publication.h"
#include "core/query/LibraryQuery.h"
#include "core/query/PublicationFilter.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <optional>
#include <utility>

// Queries run on their own threads and connections, so that the window never waits on the
// database. Each newer request replaces older ones that haven't started.
namespace papertrail::ui {

struct ListResult {
    quint64 generation = 0;
    qint64 count = 0;  // Only set by queryReady.
    qint64 offset = 0;
    QList<core::PublicationSummary> rows;
    QString error;
};

// The list of publications being shown: its total, and pages from it.
class ListQueryThread : public QThread {
    Q_OBJECT

public:
    explicit ListQueryThread(const QString &databasePath, QObject *parent = nullptr);
    ~ListQueryThread() override;

    // Replaces the query, and drops every page request for the old one. The first result holds
    // the total and the page at `offset` together, so the two can be shown at once.
    void setQuery(quint64 generation, const core::PublicationFilter &filter, const core::PublicationSort &sort,
                  qint64 offset, int limit);

    // The most recent request is served first: while scrolling, that's the page on screen.
    void requestPage(quint64 generation, qint64 offset, int limit);

signals:
    void queryReady(const papertrail::ui::ListResult &result);
    void pageReady(const papertrail::ui::ListResult &result);

protected:
    void run() override;

private:
    struct PageRequest {
        qint64 offset = 0;
        int limit = 0;
    };

    QString m_path;
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_stopping = false;
    bool m_queryPending = false;
    quint64 m_generation = 0;
    core::PublicationFilter m_filter;
    core::PublicationSort m_sort;
    PageRequest m_firstPage;
    QList<PageRequest> m_pages;
};

struct SidebarData {
    quint64 generation = 0;
    QHash<int, QList<core::FacetValue>> facets; // By core::Facet.
    QList<core::TagCount> tags;
    QList<core::CollectionCount> collections;
    core::FlagCounts flags;
    std::optional<std::pair<int, int>> years;
    QString error;
};

// The filter sidebar's counts. These take far longer than the list itself on a large library, so
// they arrive when they're ready, and a newer request abandons one in progress.
class SidebarQueryThread : public QThread {
    Q_OBJECT

public:
    explicit SidebarQueryThread(const QString &databasePath, QObject *parent = nullptr);
    ~SidebarQueryThread() override;

    void request(quint64 generation, const core::PublicationFilter &filter);

signals:
    void ready(const papertrail::ui::SidebarData &data);

protected:
    void run() override;

private:
    bool hasNewerRequest();

    QString m_path;
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_stopping = false;
    bool m_pending = false;
    quint64 m_generation = 0;
    core::PublicationFilter m_filter;
};

} // namespace papertrail::ui

Q_DECLARE_METATYPE(papertrail::ui::ListResult)
Q_DECLARE_METATYPE(papertrail::ui::SidebarData)
