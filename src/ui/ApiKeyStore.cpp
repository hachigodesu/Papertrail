#include "ui/ApiKeyStore.h"

#include <keychain.h>

namespace papertrail::ui {

const QString kClaudeKeyEntry = QStringLiteral("claude-api-key");
const QString kOpenAlexKeyEntry = QStringLiteral("openalex-api-key");
const QString kCourtListenerKeyEntry = QStringLiteral("courtlistener-api-token");
const QString kCanLiiKeyEntry = QStringLiteral("canlii-api-key");

namespace {

const QString kService = QStringLiteral("Papertrail");

// A job is never deleted while it's running, because the credential store's own code may still call back into it (as
// libsecret's does, crashing): each deletes itself when it finishes. Its answer only reaches a store that still exists.
class SystemKeyStore : public ApiKeyStore {
public:
    SystemKeyStore(const QString &entry, QObject *parent) : ApiKeyStore(parent), m_entry(entry) {}

    void read(const std::function<void(const QString &)> &done) override
    {
        if (!m_inMemory.isEmpty()) {
            done(m_inMemory);
            return;
        }
        auto *job = new QKeychain::ReadPasswordJob(kService);
        job->setAutoDelete(true);
        job->setKey(m_entry);
        connect(job, &QKeychain::Job::finished, this, [job, done] {
            done(job->error() == QKeychain::NoError ? job->textData() : QString());
        });
        job->start();
    }

    void write(const QString &key, const std::function<void(bool)> &done) override
    {
        auto *job = new QKeychain::WritePasswordJob(kService);
        job->setAutoDelete(true);
        job->setKey(m_entry);
        job->setTextData(key);
        connect(job, &QKeychain::Job::finished, this, [this, job, key, done] {
            const bool saved = job->error() == QKeychain::NoError;
            m_inMemory = saved ? QString() : key;
            done(saved);
        });
        job->start();
    }

    void clear() override
    {
        m_inMemory.clear();
        auto *job = new QKeychain::DeletePasswordJob(kService);
        job->setAutoDelete(true);
        job->setKey(m_entry);
        job->start();
    }

private:
    QString m_entry;
    QString m_inMemory;
};

} // namespace

void MemoryKeyStore::write(const QString &key, const std::function<void(bool)> &done)
{
    m_key = key;
    done(false);
}

std::unique_ptr<ApiKeyStore> createSystemKeyStore(const QString &entry, QObject *parent)
{
    return std::make_unique<SystemKeyStore>(entry, parent);
}

} // namespace papertrail::ui
