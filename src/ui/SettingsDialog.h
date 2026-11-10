#pragma once

#include <QDialog>
#include <QJsonValue>
#include <QList>
#include <QPair>

#include <functional>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QListWidgetItem;

namespace papertrail::ui {

class LibrarySession;

// The window's switches and the ways to the other settings, gathered in one place.
struct SettingsActions {
    // Checkable, and kept by the window, which acts on them. The dialog's boxes follow them both ways.
    QAction *rescanOnStartup = nullptr;
    QAction *watchFolders = nullptr;
    QAction *indexContents = nullptr;
    std::function<void()> showFolders;
    std::function<void()> showOnlineLookups;
    std::function<void()> showApiKey;
    std::function<void()> showBackups;
    // Fetches a language's model for recognising text, after asking, and says whether it's there now.
    std::function<void(const QString &code, std::function<void(bool ready)> done)> getLanguage;
    // Recognising text was turned on or off, or its languages changed.
    std::function<void()> recognitionChanged;
};

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(LibrarySession &session, const SettingsActions &actions, QWidget *parent = nullptr);

    QComboBox *themeBox() const { return m_theme; }
    QCheckBox *indexContentsBox() const { return m_indexContents; }
    QComboBox *pageLayoutBox() const { return m_pageLayout; }
    QComboBox *pageColoursBox() const { return m_pageColours; }
    QDoubleSpinBox *textSizeBox() const { return m_textSize; }
    QCheckBox *recogniseBox() const { return m_recognise; }
    QListWidget *languageList() const { return m_languages; }

private:
    QCheckBox *boxFor(QAction *action, const QString &text, const QString &explanation);
    // A list of a setting's values, saved as soon as one is chosen.
    QComboBox *choiceFor(const QString &key, const QList<QPair<QString, QString>> &labelsAndValues);
    void save(const QString &key, const QJsonValue &value);

    LibrarySession &m_session;
    QComboBox *m_theme = nullptr;
    QCheckBox *m_indexContents = nullptr;
    QComboBox *m_pageLayout = nullptr;
    QComboBox *m_pageColours = nullptr;
    QComboBox *m_ruler = nullptr;
    QDoubleSpinBox *m_textSize = nullptr;
    QCheckBox *m_recognise = nullptr;
    QListWidget *m_languages = nullptr;
    bool m_fillingLanguages = false;

    void fillLanguages();
    void onLanguageChanged(QListWidgetItem *item, const SettingsActions &actions);
    QStringList chosenLanguages() const;
};

} // namespace papertrail::ui
