#include "ui/LookupThread.h"

#include "core/db/Database.h"
#include "core/settings/SettingsStore.h"
#include "metadata/NoticeChecker.h"
#include "metadata/Transport.h"

#include <exception>
#include <memory>

namespace papertrail::ui {

using namespace metadata;

LookupThread::LookupThread(const LibraryPaths &paths, QObject *parent)
    : QThread(parent), m_paths(paths)
{
}

LookupThread::~LookupThread()
{
    quit();
    wait();
}

void LookupThread::kick()
{
    m_kickWaiting = true;
    if (m_ready)
        emit kickRequested();
}

void LookupThread::checkNotices()
{
    m_noticesWaiting = true;
    if (m_ready)
        emit noticesRequested();
}

void LookupThread::stopLookups()
{
    m_kickWaiting = false;
    if (m_ready)
        emit stopRequested();
}

void LookupThread::run()
{
    std::unique_ptr<core::Database> db;
    try {
        db = std::make_unique<core::Database>(m_paths.database);
    } catch (const std::exception &error) {
        emit problem(QString::fromUtf8(error.what()));
        return;
    }

    std::unique_ptr<Transport> transport = m_paths.transportFactory ? m_paths.transportFactory() : std::make_unique<NetworkTransport>();
    LookupEngine engine(*db, *transport);
    engine.setProviderUrls(m_paths.providerUrls);

    connect(&engine, &LookupEngine::progress, this, &LookupThread::progress);
    connect(&engine, &LookupEngine::publicationChanged, this, &LookupThread::publicationChanged);
    connect(&engine, &LookupEngine::reviewChanged, this, &LookupThread::reviewChanged);
    connect(&engine, &LookupEngine::problem, this, &LookupThread::problem);
    NoticeChecker notices(*db, *transport);
    notices.setProviderUrls(m_paths.providerUrls);
    if (m_paths.noticeCheckIntervalMs >= 0)
        notices.setIntervalMs(m_paths.noticeCheckIntervalMs);
    connect(&notices, &NoticeChecker::noticesChanged, this, &LookupThread::noticesChanged);
    connect(&notices, &NoticeChecker::problem, this, &LookupThread::problem);
    connect(&notices, &NoticeChecker::finished, this, [this](int checked, int newWarnings) {
        m_checkingNotices = false;
        emit noticesChecked(checked, newWarnings);
    });
    // Only with online lookups on, and checking too, and after any lookups, which matter more.
    const auto checkNotices = [this, &engine, &notices, &db] {
        if (engine.isRunning() || notices.isRunning() || !m_noticesWaiting)
            return;
        m_noticesWaiting = false;
        try {
            core::SettingsStore settings(*db);
            if (!settings.value(QStringLiteral("metadata.mode_chosen")).toBool()
                || settings.value(QStringLiteral("metadata.mode")).toString() == QLatin1StringView("offline")
                || !settings.value(QStringLiteral("metadata.check_notices")).toBool()) {
                return;
            }
            notices.setContactEmail(settings.value(QStringLiteral("metadata.crossref_email")).toString());
        } catch (const std::exception &) {
            return;
        }
        m_checkingNotices = true;
        notices.start();
    };
    connect(this, &LookupThread::noticesRequested, &notices, checkNotices, Qt::QueuedConnection);

    connect(&engine, &LookupEngine::finished, this, [this, checkNotices](const LookupSummary &summary) {
        m_busy = false;
        emit busyChanged(false);
        emit lookupsFinished(summary);
        checkNotices();
    });

    // A plain function run in this thread, because these signals are sent from the window's.
    const auto start = [this, &engine, &db] {
        m_kickWaiting = false;
        if (engine.isRunning())
            return;
        try {
            engine.setContactEmail(core::SettingsStore(*db).value(QStringLiteral("metadata.crossref_email")).toString());
        } catch (const std::exception &) {
            engine.setContactEmail(QString());
        }
        m_busy = true;
        emit busyChanged(true);
        engine.start();
    };
    connect(this, &LookupThread::kickRequested, &engine, start, Qt::QueuedConnection);
    connect(this, &LookupThread::stopRequested, &engine, [&engine, &notices] {
        engine.stop();
        notices.stop();
    }, Qt::QueuedConnection);

    m_ready = true;
    if (m_kickWaiting)
        start();
    else
        checkNotices();
    exec();
    m_ready = false;
    if (engine.isRunning())
        engine.stop();
    notices.stop();
}

} // namespace papertrail::ui
