#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace papertrail::core {

class Database;

enum class SettingKind {
    Value,       // Means the same on any computer.
    MachinePath, // A path on this computer, which usually won't exist on another.
    ThisComputer, // Means nothing elsewhere, such as window sizes. Never exported or imported.
};

struct SettingDefinition {
    QString key;
    QJsonValue defaultValue;
    SettingKind kind = SettingKind::Value;
    QStringList allowedValues; // Empty means any value of the default's type.
};

// The single list of settings. A setting added here is stored, validated and included in
// library exports with no further work.
const QList<SettingDefinition> &settingDefinitions();

struct SettingsImportResult {
    int applied = 0;
    QStringList ignored; // Unknown keys or invalid values, for example from a newer version.
    QMap<QString, QJsonValue> suggestedPaths; // Never applied automatically.
};

class SettingsStore {
public:
    explicit SettingsStore(Database &db) : m_db(db) {}

    // Returns the stored value, or the default. Throws std::invalid_argument for an unknown key.
    QJsonValue value(const QString &key);

    // Throws std::invalid_argument for an unknown key, wrong type or disallowed value.
    void setValue(const QString &key, const QJsonValue &value);
    void reset(const QString &key);

    // Every setting's current value, including defaults, ready for a library export. Leaves out
    // settings that belong to this computer alone.
    QJsonObject exportAll();

    // Applies settings from an export. Unusable entries are reported rather than treated as errors.
    SettingsImportResult importAll(const QJsonObject &settings);

private:
    Database &m_db;
};

} // namespace papertrail::core
