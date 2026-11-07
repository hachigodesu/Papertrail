#include "ui/ApiKeyStore.h"

#include <keychain.h>

namespace papertrail::ui {

namespace {

const QString kService = QStringLiteral("Papertrail");
const QString kEntry = QStringLiteral("claude-api-key");

class SystemKeyStore : public ApiKeyStore {
public:
    using ApiKeyStore::ApiKeyStore;

    void read(const std::function<void(const QString &)> &done) override
    {
        if (!m_inMemory.isEmpty()) {
            done(m_inMemory);
            return;
        }
        auto *job = new QKeychain::ReadPasswordJob(kService, this);
        job->setAutoDelete(true);
        job->setKey(kEntry);
        connect(job, &QKeychain::Job::finished, this, [job, done] {
            done(job->error() == QKeychain::NoError ? job->textData() : QString());
        });
        job->start();
    }

    void write(const QString &key, const std::function<void(bool)> &done) override
    {
        auto *job = new QKeychain::WritePasswordJob(kService, this);
        job->setAutoDelete(true);
        job->setKey(kEntry);
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
        auto *job = new QKeychain::DeletePasswordJob(kService, this);
        job->setAutoDelete(true);
        job->setKey(kEntry);
        job->start();
    }

private:
    QString m_inMemory;
};

} // namespace

void MemoryKeyStore::write(const QString &key, const std::function<void(bool)> &done)
{
    m_key = key;
    done(false);
}

std::unique_ptr<ApiKeyStore> createSystemKeyStore(QObject *parent)
{
    return std::make_unique<SystemKeyStore>(parent);
}

} // namespace papertrail::ui
