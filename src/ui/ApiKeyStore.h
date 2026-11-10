#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace papertrail::ui {

// Where a key of the user's own is kept, such as the Claude API key or an OpenAlex key. It's never in the database or
// the settings, because exports include every setting, and it's never written to a log.
class ApiKeyStore : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Both answer later, since the operating system's store may have to ask the user something.
    virtual void read(const std::function<void(const QString &key)> &done) = 0;
    virtual void write(const QString &key, const std::function<void(bool savedPermanently)> &done) = 0;
    virtual void clear() = 0;
};

// Only for as long as the app is open.
class MemoryKeyStore : public ApiKeyStore {
    Q_OBJECT

public:
    using ApiKeyStore::ApiKeyStore;
    void read(const std::function<void(const QString &)> &done) override { done(m_key); }
    void write(const QString &key, const std::function<void(bool)> &done) override;
    void clear() override { m_key.clear(); }

private:
    QString m_key;
};

// The names keys are kept under in the credential store.
extern const QString kClaudeKeyEntry;
extern const QString kOpenAlexKeyEntry;

// The operating system's credential store, holding the key named `entry`. If there isn't one, as on a Linux system
// with no secret service, the key is held in memory for this session instead, and whoever's saving it is told.
std::unique_ptr<ApiKeyStore> createSystemKeyStore(const QString &entry, QObject *parent = nullptr);

} // namespace papertrail::ui
