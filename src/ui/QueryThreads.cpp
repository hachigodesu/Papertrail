#include "ui/QueryThreads.h"

#include "core/db/Database.h"
#include "core/repo/FullTextIndex.h"

#include <QMutexLocker>

#include <exception>
#include <memory>

namespace papertrail::ui {

using namespace core;

ListQueryThread::ListQueryThread(const QString &databasePath, QObject *parent)
    : QThread(parent), m_path(databasePath)
{
    qRegisterMetaType<ListResult>();
}

ListQueryThread::~ListQueryThread()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_wake.wakeAll();
    }
    wait();
}

void ListQueryThread::setQuery(quint64 generation, const PublicationFilter &filter, const PublicationSort &sort,
                               qint64 offset, int limit)
{
    QMutexLocker lock(&m_mutex);
    m_generation = generation;
    m_filter = filter;
    m_sort = sort;
    m_firstPage = {offset, limit};
    m_queryPending = true;
    m_pages.clear();
    m_wake.wakeAll();
}

void ListQueryThread::requestPage(quint64 generation, qint64 offset, int limit)
{
    QMutexLocker lock(&m_mutex);
    if (generation != m_generation)
        return;
    m_pages << PageRequest{offset, limit};
    m_wake.wakeAll();
}

void ListQueryThread::run()
{
    std::unique_ptr<Database> db;
    std::unique_ptr<LibraryQuery> query;
    QString openError;
    try {
        db = std::make_unique<Database>(m_path);
        attachFullText(*db, fullTextPathFor(m_path));
        query = std::make_unique<LibraryQuery>(*db);
    } catch (const std::exception &error) {
        openError = QString::fromUtf8(error.what());
    }

    while (true) {
        bool isQuery = false;
        quint64 generation = 0;
        PublicationFilter filter;
        PublicationSort sort;
        PageRequest page;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_stopping && !m_queryPending && m_pages.isEmpty())
                m_wake.wait(&m_mutex);
            if (m_stopping)
                return;
            generation = m_generation;
            filter = m_filter;
            sort = m_sort;
            isQuery = m_queryPending;
            if (isQuery) {
                page = m_firstPage;
                m_queryPending = false;
            } else {
                page = m_pages.takeLast();
            }
        }

        ListResult result;
        result.generation = generation;
        result.offset = page.offset;
        result.error = openError;
        if (query) {
            try {
                if (isQuery)
                    result.count = query->count(filter);
                result.rows = query->page(filter, sort, page.offset, page.limit);
            } catch (const std::exception &error) {
                result.error = QString::fromUtf8(error.what());
            }
        }
        if (isQuery)
            emit queryReady(result);
        else
            emit pageReady(result);
    }
}

SidebarQueryThread::SidebarQueryThread(const QString &databasePath, QObject *parent)
    : QThread(parent), m_path(databasePath)
{
    qRegisterMetaType<SidebarData>();
}

SidebarQueryThread::~SidebarQueryThread()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_wake.wakeAll();
    }
    wait();
}

void SidebarQueryThread::request(quint64 generation, const PublicationFilter &filter)
{
    QMutexLocker lock(&m_mutex);
    m_generation = generation;
    m_filter = filter;
    m_pending = true;
    m_wake.wakeAll();
}

bool SidebarQueryThread::hasNewerRequest()
{
    QMutexLocker lock(&m_mutex);
    return m_pending || m_stopping;
}

void SidebarQueryThread::run()
{
    std::unique_ptr<Database> db;
    std::unique_ptr<LibraryQuery> query;
    QString openError;
    try {
        db = std::make_unique<Database>(m_path);
        attachFullText(*db, fullTextPathFor(m_path));
        query = std::make_unique<LibraryQuery>(*db);
    } catch (const std::exception &error) {
        openError = QString::fromUtf8(error.what());
    }

    static const QList<Facet> facets = {Facet::Author,     Facet::Journal,  Facet::Publisher, Facet::Conference,
                                        Facet::Language,   Facet::Format,   Facet::ReadStatus};

    while (true) {
        SidebarData data;
        PublicationFilter filter;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_stopping && !m_pending)
                m_wake.wait(&m_mutex);
            if (m_stopping)
                return;
            data.generation = m_generation;
            filter = m_filter;
            m_pending = false;
        }

        data.error = openError;
        bool abandoned = false;
        if (query) {
            try {
                data.tags = query->tagCounts(filter);
                data.collections = query->collectionCounts(filter);
                abandoned = hasNewerRequest();
                if (!abandoned) {
                    data.flags = query->flagCounts(filter);
                    data.years = query->yearRange(filter);
                }
                for (const Facet facet : facets) {
                    if (abandoned || (abandoned = hasNewerRequest()))
                        break;
                    data.facets.insert(int(facet), query->facet(filter, facet));
                }
            } catch (const std::exception &error) {
                data.error = QString::fromUtf8(error.what());
            }
        }
        if (!abandoned)
            emit ready(data);
    }
}

} // namespace papertrail::ui
