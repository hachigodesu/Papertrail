#include "core/settings/SettingsStore.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

#include <stdexcept>

namespace papertrail::core {

namespace {

const SettingDefinition *findDefinition(const QString &key)
{
    for (const SettingDefinition &definition : settingDefinitions()) {
        if (definition.key == key)
            return &definition;
    }
    return nullptr;
}

const SettingDefinition &requireDefinition(const QString &key)
{
    const SettingDefinition *definition = findDefinition(key);
    if (!definition)
        throw std::invalid_argument("unknown setting: " + key.toStdString());
    return *definition;
}

bool isValid(const SettingDefinition &definition, const QJsonValue &value)
{
    if (value.type() != definition.defaultValue.type())
        return false;
    return definition.allowedValues.isEmpty() || definition.allowedValues.contains(value.toString());
}

// QJsonDocument only stores objects and arrays, so each value is held in a one-item array.
QString encode(const QJsonValue &value)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

QJsonValue decode(const QString &text)
{
    return QJsonDocument::fromJson(text.toUtf8()).array().at(0);
}

} // namespace

const QList<SettingDefinition> &settingDefinitions()
{
    using K = SettingKind;
    static const QList<SettingDefinition> definitions = {
        {QStringLiteral("metadata.mode"), QStringLiteral("online_review"), K::Value,
         {QStringLiteral("online_review"), QStringLiteral("online_auto"), QStringLiteral("offline")}},
        {QStringLiteral("metadata.crossref_email"), QStringLiteral(""), K::Value, {}},
        {QStringLiteral("library.view"), QStringLiteral("grid"), K::Value,
         {QStringLiteral("grid"), QStringLiteral("list")}},
        {QStringLiteral("library.sort_field"), QStringLiteral("title"), K::Value,
         {QStringLiteral("title"), QStringLiteral("author"), QStringLiteral("year"), QStringLiteral("added")}},
        {QStringLiteral("library.sort_ascending"), true, K::Value, {}},
        {QStringLiteral("library.rescan_on_startup"), true, K::Value, {}},
        {QStringLiteral("library.watch_folders"), true, K::Value, {}},
        {QStringLiteral("library.show_filters"), true, K::Value, {}},
        {QStringLiteral("library.show_details"), true, K::Value, {}},
        {QStringLiteral("library.last_import_folder"), QStringLiteral(""), K::MachinePath, {}},
        {QStringLiteral("window.geometry"), QStringLiteral(""), K::ThisComputer, {}},
        {QStringLiteral("window.splitter"), QStringLiteral(""), K::ThisComputer, {}},
        {QStringLiteral("window.list_columns"), QStringLiteral(""), K::ThisComputer, {}},
        {QStringLiteral("reader.page_layout"), QStringLiteral("continuous"), K::Value,
         {QStringLiteral("single"), QStringLiteral("two_page"), QStringLiteral("continuous")}},
        {QStringLiteral("reader.page_colours"), QStringLiteral("light"), K::Value,
         {QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("sepia")}},
        {QStringLiteral("reader.font_size"), 12.0, K::Value, {}},
        {QStringLiteral("citation.style"), QStringLiteral("harvard"), K::Value,
         {QStringLiteral("harvard"), QStringLiteral("apa"), QStringLiteral("ieee"), QStringLiteral("chicago")}},
    };
    return definitions;
}

QJsonValue SettingsStore::value(const QString &key)
{
    const SettingDefinition &definition = requireDefinition(key);
    auto select = m_db.prepare("SELECT value FROM setting WHERE key = ?");
    select->bind(1, key);
    if (!select->step())
        return definition.defaultValue;
    return decode(select->textAt(0));
}

void SettingsStore::setValue(const QString &key, const QJsonValue &value)
{
    const SettingDefinition &definition = requireDefinition(key);
    if (!isValid(definition, value))
        throw std::invalid_argument("invalid value for setting: " + key.toStdString());

    auto upsert = m_db.prepare(
        "INSERT INTO setting (key, value, updated_at) VALUES (?, ?, ?) "
        "ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at");
    upsert->bind(1, key);
    upsert->bind(2, encode(value));
    upsert->bind(3, QDateTime::currentMSecsSinceEpoch());
    upsert->step();
}

void SettingsStore::reset(const QString &key)
{
    requireDefinition(key);
    auto remove = m_db.prepare("DELETE FROM setting WHERE key = ?");
    remove->bind(1, key);
    remove->step();
}

QJsonObject SettingsStore::exportAll()
{
    QJsonObject settings;
    for (const SettingDefinition &definition : settingDefinitions()) {
        if (definition.kind != SettingKind::ThisComputer)
            settings.insert(definition.key, value(definition.key));
    }
    return settings;
}

SettingsImportResult SettingsStore::importAll(const QJsonObject &settings)
{
    SettingsImportResult result;
    Transaction transaction(m_db);
    for (auto entry = settings.begin(); entry != settings.end(); ++entry) {
        const SettingDefinition *definition = findDefinition(entry.key());
        if (!definition || definition->kind == SettingKind::ThisComputer
            || !isValid(*definition, entry.value())) {
            result.ignored << entry.key();
        } else if (definition->kind == SettingKind::MachinePath) {
            result.suggestedPaths.insert(entry.key(), entry.value());
        } else {
            setValue(entry.key(), entry.value());
            ++result.applied;
        }
    }
    transaction.commit();
    return result;
}

} // namespace papertrail::core
