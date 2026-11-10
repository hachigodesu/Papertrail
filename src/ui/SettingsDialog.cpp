#include "ui/SettingsDialog.h"

#include "core/settings/SettingsStore.h"
#include "metadata/OcrLanguages.h"
#include "ui/LibrarySession.h"
#include "ui/Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QLocale>
#include <QPointer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

namespace {

QPushButton *buttonFor(const QString &text, const QString &explanation, const std::function<void()> &open, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setToolTip(explanation);
    button->setEnabled(static_cast<bool>(open));
    QObject::connect(button, &QPushButton::clicked, parent, [open] {
        if (open)
            open();
    });
    return button;
}

} // namespace

SettingsDialog::SettingsDialog(LibrarySession &session, const SettingsActions &actions, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(tr("Settings"));
    auto *layout = new QVBoxLayout(this);

    auto *appearance = new QGroupBox(tr("Appearance"), this);
    auto *appearanceForm = new QFormLayout(appearance);
    m_theme = new QComboBox(appearance);
    m_theme->addItem(tr("Same as the system"), settingFor(Theme::System));
    m_theme->addItem(tr("Light"), settingFor(Theme::Light));
    m_theme->addItem(tr("Dark"), settingFor(Theme::Dark));
    m_theme->setToolTip(tr("The window's colours. Pages in the reader keep the colours chosen there."));
    QString stored;
    try {
        stored = m_session.settings().value(QStringLiteral("appearance.theme")).toString();
    } catch (const std::exception &) {
        // Shown as the default.
    }
    m_theme->setCurrentIndex(qMax(0, m_theme->findData(settingFor(themeFromSetting(stored)))));
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this] {
        const QString chosen = m_theme->currentData().toString();
        try {
            m_session.settings().setValue(QStringLiteral("appearance.theme"), chosen);
        } catch (const std::exception &error) {
            QMessageBox::warning(this, tr("Settings"), tr("The theme couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        }
        applyTheme(themeFromSetting(chosen));
    });
    appearanceForm->addRow(tr("Theme:"), m_theme);
    layout->addWidget(appearance);

    // The reader keeps what was last chosen in a document, so these are that too.
    auto *reading = new QGroupBox(tr("Reading"), this);
    auto *readingForm = new QFormLayout(reading);
    auto *readingNote = new QLabel(tr("Documents open with these. Changing them while reading changes them here too."), reading);
    readingNote->setWordWrap(true);
    readingForm->addRow(readingNote);
    m_pageLayout = choiceFor(QStringLiteral("reader.page_layout"),
                             {{tr("Continuous"), QStringLiteral("continuous")},
                              {tr("Single page"), QStringLiteral("single")},
                              {tr("Two pages"), QStringLiteral("two_page")}});
    readingForm->addRow(tr("Page layout:"), m_pageLayout);
    m_pageColours = choiceFor(QStringLiteral("reader.page_colours"),
                              {{tr("Light"), QStringLiteral("light")},
                               {tr("Dark"), QStringLiteral("dark")},
                               {tr("Sepia"), QStringLiteral("sepia")}});
    m_pageColours->setToolTip(tr("Dark pages leave pictures in their own colours."));
    readingForm->addRow(tr("Page colours:"), m_pageColours);
    m_textSize = new QDoubleSpinBox(reading);
    m_textSize->setRange(8, 32);
    m_textSize->setDecimals(0);
    m_textSize->setSuffix(tr(" pt"));
    m_textSize->setToolTip(tr("For EPUB and other books whose text flows to fit the page."));
    try {
        m_textSize->setValue(m_session.settings().value(QStringLiteral("reader.font_size")).toDouble(12));
    } catch (const std::exception &) {
        m_textSize->setValue(12);
    }
    connect(m_textSize, &QDoubleSpinBox::valueChanged, this, [this](double size) { save(QStringLiteral("reader.font_size"), size); });
    readingForm->addRow(tr("Text size:"), m_textSize);
    m_ruler = choiceFor(QStringLiteral("reader.ruler"), {{tr("A line"), QStringLiteral("line")}, {tr("A paragraph"), QStringLiteral("paragraph")}});
    m_ruler->setToolTip(tr("View > Reading ruler dims all but this, moved with Up and Down."));
    readingForm->addRow(tr("Reading ruler marks:"), m_ruler);
    layout->addWidget(reading);

    auto *library = new QGroupBox(tr("Library"), this);
    auto *libraryLayout = new QVBoxLayout(library);
    libraryLayout->addWidget(boxFor(actions.rescanOnStartup, tr("Scan folders when Papertrail starts"),
                                    tr("Looks for new, changed and moved files each time Papertrail opens.")));
    libraryLayout->addWidget(boxFor(actions.watchFolders, tr("Watch folders for changes"),
                                    tr("Notices files being added, changed or moved while Papertrail is open.")));
    m_indexContents = boxFor(actions.indexContents, tr("Index document text"),
                             tr("Reads documents' text so that it can be searched. It stays on this computer, and your files aren't changed."));
    libraryLayout->addWidget(m_indexContents);
    auto *libraryButtons = new QHBoxLayout;
    libraryButtons->addWidget(buttonFor(tr("Library folders..."), tr("The folders Papertrail indexes"), actions.showFolders, library));
    libraryButtons->addWidget(buttonFor(tr("Backups..."), tr("Daily backups of your library's details, and restoring one"),
                                        actions.showBackups, library));
    libraryButtons->addStretch(1);
    libraryLayout->addLayout(libraryButtons);
    layout->addWidget(library);

    // Only where this copy can recognise text at all.
    if (canRecogniseText()) {
        auto *scanned = new QGroupBox(tr("Scanned documents"), this);
        auto *scannedLayout = new QVBoxLayout(scanned);
        m_recognise = new QCheckBox(tr("Recognise the text of scanned documents"), scanned);
        m_recognise->setToolTip(tr("Reads the words in documents that are only pictures of pages, so they can be searched, "
                                   "selected and highlighted. It works in the background, slowly, and only on this computer. "
                                   "Your files aren't changed."));
        try {
            m_recognise->setChecked(m_session.settings().value(QStringLiteral("ocr.enabled")).toBool());
        } catch (const std::exception &) {
        }
        connect(m_recognise, &QCheckBox::toggled, this, [this, actions](bool on) {
            save(QStringLiteral("ocr.enabled"), on);
            if (actions.recognitionChanged)
                actions.recognitionChanged();
        });
        scannedLayout->addWidget(m_recognise);
        auto *languagesNote = new QLabel(tr("Languages to read them in. Those not here yet are fetched when chosen."), scanned);
        languagesNote->setWordWrap(true);
        scannedLayout->addWidget(languagesNote);
        m_languages = new QListWidget(scanned);
        m_languages->setAccessibleName(tr("Languages to recognise"));
        m_languages->setMaximumHeight(140);
        scannedLayout->addWidget(m_languages);
        fillLanguages();
        connect(m_languages, &QListWidget::itemChanged, this, [this, actions](QListWidgetItem *item) { onLanguageChanged(item, actions); });
        layout->addWidget(scanned);
    }

    auto *online = new QGroupBox(tr("Online"), this);
    auto *onlineLayout = new QHBoxLayout(online);
    onlineLayout->addWidget(buttonFor(tr("Online lookups..."), tr("Whether and how details are looked up online"),
                                      actions.showOnlineLookups, online));
    onlineLayout->addWidget(buttonFor(tr("Claude API key..."), tr("The key used for summaries from Claude"), actions.showApiKey, online));
    onlineLayout->addStretch(1);
    layout->addWidget(online);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::fillLanguages()
{
    m_fillingLanguages = true;
    m_languages->clear();
    QStringList chosen;
    try {
        chosen = m_session.settings().value(QStringLiteral("ocr.languages")).toString().split(u'+', Qt::SkipEmptyParts);
    } catch (const std::exception &) {
    }
    const QDir tessdata(m_session.paths().tessdataDirectory);
    for (const metadata::OcrLanguage &language : metadata::ocrLanguages()) {
        const QString code = QString::fromLatin1(language.code);
        const bool here = QFileInfo::exists(tessdata.filePath(code + QStringLiteral(".traineddata"))) || code == QLatin1StringView("eng");
        const QString name = QCoreApplication::translate("Languages", language.name);
        auto *item = new QListWidgetItem(here ? name
                                              : tr("%1 (to fetch, %2 MB)").arg(name).arg(QLocale().toString(double(language.bytes) / 1e6, 'f', 1)),
                                         m_languages);
        item->setData(Qt::UserRole, code);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(chosen.contains(code) ? Qt::Checked : Qt::Unchecked);
    }
    m_fillingLanguages = false;
}

QStringList SettingsDialog::chosenLanguages() const
{
    QStringList codes;
    for (int row = 0; row < m_languages->count(); ++row) {
        if (m_languages->item(row)->checkState() == Qt::Checked)
            codes << m_languages->item(row)->data(Qt::UserRole).toString();
    }
    return codes;
}

void SettingsDialog::onLanguageChanged(QListWidgetItem *item, const SettingsActions &actions)
{
    if (m_fillingLanguages)
        return;
    const QString code = item->data(Qt::UserRole).toString();
    const bool here = code == QLatin1StringView("eng")
                      || QFileInfo::exists(QDir(m_session.paths().tessdataDirectory).filePath(code + QStringLiteral(".traineddata")));
    const auto choose = [this, actions] {
        QStringList codes = chosenLanguages();
        // At least one, or nothing could be read.
        if (codes.isEmpty())
            codes << QStringLiteral("eng");
        save(QStringLiteral("ocr.languages"), codes.join(u'+'));
        if (actions.recognitionChanged)
            actions.recognitionChanged();
    };
    if (item->checkState() != Qt::Checked || here) {
        choose();
        return;
    }
    // Not here yet: unchosen until it's been fetched.
    m_fillingLanguages = true;
    item->setCheckState(Qt::Unchecked);
    m_fillingLanguages = false;
    if (!actions.getLanguage)
        return;
    QPointer<SettingsDialog> self(this);
    actions.getLanguage(code, [self, code, choose](bool ready) {
        if (!self || !ready)
            return;
        self->fillLanguages();
        for (int row = 0; row < self->m_languages->count(); ++row) {
            QListWidgetItem *fetched = self->m_languages->item(row);
            if (fetched->data(Qt::UserRole).toString() == code) {
                self->m_fillingLanguages = true;
                fetched->setCheckState(Qt::Checked);
                self->m_fillingLanguages = false;
            }
        }
        choose();
    });
}

QComboBox *SettingsDialog::choiceFor(const QString &key, const QList<QPair<QString, QString>> &labelsAndValues)
{
    auto *box = new QComboBox(this);
    for (const auto &[label, value] : labelsAndValues)
        box->addItem(label, value);
    QString stored;
    try {
        stored = m_session.settings().value(key).toString();
    } catch (const std::exception &) {
        // Shown as the first.
    }
    box->setCurrentIndex(qMax(0, box->findData(stored)));
    connect(box, &QComboBox::currentIndexChanged, this, [this, box, key] { save(key, box->currentData().toString()); });
    return box;
}

void SettingsDialog::save(const QString &key, const QJsonValue &value)
{
    try {
        m_session.settings().setValue(key, value);
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Settings"), tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
    }
}

QCheckBox *SettingsDialog::boxFor(QAction *action, const QString &text, const QString &explanation)
{
    auto *box = new QCheckBox(text, this);
    box->setToolTip(explanation);
    if (!action) {
        box->setEnabled(false);
        return box;
    }
    box->setChecked(action->isChecked());
    // The window acts on its action, so the box only passes changes along, both ways.
    connect(box, &QCheckBox::toggled, action, &QAction::setChecked);
    connect(action, &QAction::toggled, box, &QCheckBox::setChecked);
    return box;
}

} // namespace papertrail::ui
