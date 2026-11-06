#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/settings/SettingsStore.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail::core;

class SettingsTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        m_settings = std::make_unique<SettingsStore>(*m_db);
    }

    void cleanup()
    {
        m_settings.reset();
        m_db.reset();
    }

    void defaultsApplyUntilSet()
    {
        QCOMPARE(m_settings->value(QStringLiteral("metadata.mode")).toString(), QStringLiteral("online_review"));
        m_settings->setValue(QStringLiteral("metadata.mode"), QStringLiteral("offline"));
        QCOMPARE(m_settings->value(QStringLiteral("metadata.mode")).toString(), QStringLiteral("offline"));
        m_settings->reset(QStringLiteral("metadata.mode"));
        QCOMPARE(m_settings->value(QStringLiteral("metadata.mode")).toString(), QStringLiteral("online_review"));
    }

    void valuesPersistAcrossConnections()
    {
        m_settings->setValue(QStringLiteral("library.sort_ascending"), false);
        m_settings.reset();
        m_db.reset();

        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        m_settings = std::make_unique<SettingsStore>(*m_db);
        QCOMPARE(m_settings->value(QStringLiteral("library.sort_ascending")).toBool(), false);
    }

    void allMetadataModesAreAccepted()
    {
        for (const char *mode : {"online_review", "online_auto", "offline"})
            m_settings->setValue(QStringLiteral("metadata.mode"), QString::fromLatin1(mode));
    }

    void harvardIsTheDefaultCitationStyle()
    {
        QCOMPARE(m_settings->value(QStringLiteral("citation.style")).toString(), QStringLiteral("harvard"));
    }

    void badKeysAndValuesAreRefused()
    {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_settings->value(QStringLiteral("no.such.key")));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_settings->setValue(QStringLiteral("no.such.key"), 1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_settings->setValue(QStringLiteral("metadata.mode"), QStringLiteral("nonsense")));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_settings->setValue(QStringLiteral("library.sort_ascending"), QStringLiteral("yes")));
    }

    void exportIncludesEveryDefinedSetting()
    {
        m_settings->setValue(QStringLiteral("library.view"), QStringLiteral("list"));
        const QJsonObject exported = m_settings->exportAll();
        for (const SettingDefinition &definition : settingDefinitions()) {
            const bool expected = definition.kind != SettingKind::ThisComputer;
            QVERIFY2(exported.contains(definition.key) == expected, qPrintable(definition.key));
        }
        QCOMPARE(exported.value(QStringLiteral("library.view")).toString(), QStringLiteral("list"));
    }

    void importAppliesValuesAndReportsTheRest()
    {
        QJsonObject incoming;
        incoming.insert(QStringLiteral("library.view"), QStringLiteral("list"));
        incoming.insert(QStringLiteral("citation.style"), QStringLiteral("ieee"));
        incoming.insert(QStringLiteral("metadata.mode"), QStringLiteral("not_a_mode"));
        incoming.insert(QStringLiteral("from.a.newer.version"), 1);
        incoming.insert(QStringLiteral("library.last_import_folder"), QStringLiteral("C:/Users/Someone/Papers"));

        const SettingsImportResult result = m_settings->importAll(incoming);

        QCOMPARE(result.applied, 2);
        QCOMPARE(m_settings->value(QStringLiteral("library.view")).toString(), QStringLiteral("list"));
        QCOMPARE(m_settings->value(QStringLiteral("citation.style")).toString(), QStringLiteral("ieee"));
        QCOMPARE(m_settings->value(QStringLiteral("metadata.mode")).toString(), QStringLiteral("online_review"));
        QVERIFY(result.ignored.contains(QStringLiteral("from.a.newer.version")));
        QVERIFY(result.ignored.contains(QStringLiteral("metadata.mode")));

        // A path from another computer is offered as a suggestion, and never applied.
        QCOMPARE(result.suggestedPaths.value(QStringLiteral("library.last_import_folder")).toString(),
                 QStringLiteral("C:/Users/Someone/Papers"));
        QCOMPARE(m_settings->value(QStringLiteral("library.last_import_folder")).toString(), QString());
    }

    void exportImportRoundTrips()
    {
        m_settings->setValue(QStringLiteral("reader.page_colours"), QStringLiteral("sepia"));
        m_settings->setValue(QStringLiteral("library.rescan_on_startup"), false);
        const QJsonObject exported = m_settings->exportAll();

        QTemporaryDir otherDir;
        Database other(otherDir.filePath(QStringLiteral("other.db")));
        migrateLibrary(other);
        SettingsStore otherSettings(other);
        otherSettings.importAll(exported);

        QCOMPARE(otherSettings.value(QStringLiteral("reader.page_colours")).toString(), QStringLiteral("sepia"));
        QCOMPARE(otherSettings.value(QStringLiteral("library.rescan_on_startup")).toBool(), false);
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<SettingsStore> m_settings;
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "test_settings.moc"
