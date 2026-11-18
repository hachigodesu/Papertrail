#include "citation/CslThread.h"

#include "citation/CslEngine.h"

#include <QPointer>
#include <QThread>

#include <exception>

namespace papertrail::citation {

CslThread::CslThread(QObject *parent) : QObject(parent)
{
    m_thread = QThread::create([this] { run(); });
    // citeproc-js recurses deeply, and QuickJS checks its own stack against less than this.
    m_thread->setStackSize(uint(CslEngine::kCslStackBytes));
    m_thread->setObjectName(QStringLiteral("CslThread"));
    m_thread->start(QThread::LowPriority);
}

CslThread::~CslThread()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_waiting.clear();
        if (m_current)
            m_current->cancelled->store(true);
    }
    m_wake.notify_all();
    m_thread->wait();
    delete m_thread;
}

qint64 CslThread::render(const QString &channel, Kind kind, const CslLayout &layout, const QList<CitationItem> &items,
                         const StyleOptions &options)
{
    Request request;
    request.channel = channel;
    request.kind = kind;
    request.layout = layout;
    request.items = items;
    request.options = options;
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(m_mutex);
        request.id = ++m_next;
        std::erase_if(m_waiting, [&](const Request &waiting) { return waiting.channel == channel; });
        if (m_current && m_current->channel == channel)
            m_current->cancelled->store(true);
        m_waiting.push_back(std::move(request));
    }
    m_wake.notify_all();
    return m_next;
}

void CslThread::cancel(const QString &channel)
{
    std::lock_guard lock(m_mutex);
    std::erase_if(m_waiting, [&](const Request &waiting) { return waiting.channel == channel; });
    if (m_current && m_current->channel == channel)
        m_current->cancelled->store(true);
}

void CslThread::setLimits(std::chrono::milliseconds base, std::chrono::milliseconds perItem)
{
    std::lock_guard lock(m_mutex);
    m_base = base;
    m_perItem = perItem;
    m_limitsChanged = true;
}

void CslThread::run()
{
    CslEngine engine;
    QPointer<CslThread> self(this);
    while (true) {
        std::shared_ptr<Request> request;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_waiting.empty(); });
            if (m_stopping)
                return;
            request = std::make_shared<Request>(std::move(m_waiting.front()));
            m_waiting.pop_front();
            m_current = request;
            if (m_limitsChanged) {
                engine.setLimits(m_base, m_perItem);
                m_limitsChanged = false;
            }
        }
        engine.setCancelFlag(request->cancelled);
        QList<RichText> entries;
        QList<int> order;
        QString error;
        try {
            if (engine.useStyle(request->layout, &error)) {
                if (request->kind == Kind::References) {
                    if (const std::optional<FormattedReferences> references = engine.references(request->items, request->options, &error)) {
                        entries = references->entries;
                        order = references->order;
                    }
                } else if (const std::optional<RichText> citation = engine.citation(request->items, request->options, &error)) {
                    entries << *citation;
                }
            }
        } catch (const std::exception &failure) {
            error = QString::fromUtf8(failure.what());
        }
        const bool cancelled = request->cancelled->load();
        {
            std::lock_guard lock(m_mutex);
            m_current.reset();
        }
        if (cancelled)
            continue;
        if (error.isEmpty() && entries.isEmpty() && !request->items.isEmpty())
            error = tr("The style gave nothing for these.");
        const qint64 id = request->id;
        QMetaObject::invokeMethod(this, [self, id, entries, order, error] {
            if (self)
                emit self->finished(id, entries, order, error);
        }, Qt::QueuedConnection);
    }
}

} // namespace papertrail::citation
