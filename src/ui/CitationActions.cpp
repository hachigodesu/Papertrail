#include "ui/CitationActions.h"

#include "citation/CslThread.h"
#include "core/db/Database.h"
#include "core/repo/CslStyleRepository.h"
#include "ui/CitationStylesDialog.h"
#include "ui/LibrarySession.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <exception>

namespace papertrail::ui {

using namespace citation;

CitationActions::CitationActions(LibrarySession &session, QWidget *window)
    : QObject(window), m_session(session), m_window(window)
{
}

CitationActions::~CitationActions()
{
    if (m_thread) {
        if (m_cancel)
            m_cancel->store(true);
        m_thread->wait();
    }
}

QString CitationActions::styleKey() const
{
    try {
        return m_session.settings().value(QStringLiteral("citation.style")).toString();
    } catch (const std::exception &) {
        return citation::styleKey(CitationStyle::Harvard);
    }
}

void CitationActions::setStyleKey(const QString &key)
{
    try {
        m_session.settings().setValue(QStringLiteral("citation.style"), key);
        if (isCslStyleKey(key)) {
            QStringList recent = recentStyleKeys();
            recent.removeAll(key);
            recent.prepend(key);
            m_session.settings().setValue(QStringLiteral("citation.recent_styles"), recent.mid(0, kRecentStyles).join(u'\n'));
        }
        emit styleChanged(key);
    } catch (const std::exception &error) {
        emit message(tr("The citation style couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
    }
}

CitationStyle CitationActions::style() const
{
    return styleFromKey(styleKey()).value_or(CitationStyle::Harvard);
}

void CitationActions::setStyle(CitationStyle chosen)
{
    setStyleKey(citation::styleKey(chosen));
}

QString CitationActions::styleTitle(const QString &key) const
{
    if (const std::optional<CitationStyle> builtIn = styleFromKey(key))
        return styleName(*builtIn);
    const QString id = cslStyleId(key);
    for (const CslStyleInfo &style : bundledCslStyles()) {
        if (style.id == id)
            return style.title;
    }
    try {
        if (const std::optional<core::AddedCslStyle> added = core::CslStyleRepository(m_session.database()).get(id))
            return added->title;
    } catch (const std::exception &) {
    }
    return id.isEmpty() ? styleName(CitationStyle::Harvard) : id;
}

QStringList CitationActions::recentStyleKeys() const
{
    QStringList recent;
    try {
        for (const QString &key : m_session.settings().value(QStringLiteral("citation.recent_styles")).toString().split(u'\n')) {
            if (isCslStyleKey(key) && !recent.contains(key))
                recent << key;
        }
    } catch (const std::exception &) {
    }
    return recent.mid(0, kRecentStyles);
}

std::optional<CslLayout> CitationActions::cslLayout(const QString &key, QString *error) const
{
    QString locale;
    try {
        locale = m_session.settings().value(QStringLiteral("citation.locale")).toString();
    } catch (const std::exception &) {
    }
    core::Database &db = m_session.database();
    return resolveCslStyle(cslStyleId(key), locale, [&db](const QString &id) {
        try {
            return core::CslStyleRepository(db).xml(id);
        } catch (const std::exception &) {
            return QByteArray();
        }
    }, error);
}

CslThread *CitationActions::cslThread()
{
    if (!m_csl) {
        m_csl = new CslThread(this);
        connect(m_csl, &CslThread::finished, this, &CitationActions::onCslFinished);
    }
    return m_csl;
}

StyleOptions CitationActions::styleOptions() const
{
    StyleOptions options;
    options.accessed = QDate::currentDate();
    return options;
}

QList<CitationItem> CitationActions::itemsFor(const QList<qint64> &ids)
{
    try {
        return citationItems(m_session.database(), ids);
    } catch (const std::exception &error) {
        emit message(tr("Those couldn't be read: %1").arg(QString::fromUtf8(error.what())));
        return {};
    }
}

QMenu *CitationActions::addCiteMenu(QMenu *menu, qint64 count, const IdSource &ids, bool onlyCases)
{
    QMenu *cite = menu->addMenu(tr("Cite"));
    fillCiteMenu(cite, count, ids, onlyCases);
    return cite;
}

void CitationActions::fillCiteMenu(QMenu *menu, qint64 count, const IdSource &ids, bool onlyCases)
{
    const bool canCopy = count > 0 && count <= kMaxToCopy;
    if (onlyCases) {
        // A case is cited in a legal form, whatever style the rest of a list is in.
        for (const bool oscola : {false, true}) {
            const QString form = oscola ? tr("OSCOLA") : tr("Bluebook");
            QAction *copy = menu->addAction(count == 1 ? tr("Copy citation (%1)").arg(form) : tr("Copy %1 citations (%2)").arg(count).arg(form),
                                            this, [this, ids, oscola] { copyCaseCitations(ids(), oscola); });
            copy->setObjectName(oscola ? QStringLiteral("copyOscola") : QStringLiteral("copyBluebook"));
            copy->setEnabled(canCopy);
        }
        menu->addSeparator();
        QAction *exporting = menu->addAction(count == 1 ? tr("Export citation...") : tr("Export %1 citations...").arg(count), this,
                                             [this, ids] { askAndExport(ids()); });
        exporting->setEnabled(count > 0 && !isExporting());
        return;
    }
    const QString current = styleKey();
    const QString name = styleTitle(current);
    // A note style's citation is its footnote.
    bool note = false;
    if (isCslStyleKey(current)) {
        if (const std::optional<CslLayout> layout = cslLayout(current))
            note = readCslStyle(layout->xml).value_or(CslStyleInfo{}).isNote;
    }
    QAction *references = menu->addAction(count == 1 ? tr("Copy reference (%1)").arg(name)
                                                     : tr("Copy %1 references (%2)").arg(count).arg(name),
                                          this, [this, ids] { copyReferences(ids()); });
    references->setObjectName(QStringLiteral("copyReferences"));
    QAction *inText = menu->addAction(note ? tr("Copy note (%1)").arg(name) : tr("Copy in-text citation (%1)").arg(name), this,
                                      [this, ids] { copyInTextCitation(ids()); });
    inText->setObjectName(QStringLiteral("copyInText"));
    references->setEnabled(canCopy);
    inText->setEnabled(canCopy);
    if (count > kMaxToCopy)
        references->setToolTip(tr("Too many to copy at once. Export them to a file instead."));

    QMenu *styles = menu->addMenu(tr("Style"));
    styles->setObjectName(QStringLiteral("citationStyles"));
    auto *group = new QActionGroup(styles);
    group->setExclusive(true);
    const auto offer = [&](const QString &key) {
        QAction *action = styles->addAction(styleTitle(key), this, [this, key] { setStyleKey(key); });
        action->setCheckable(true);
        action->setChecked(key == current);
        action->setData(key);
        group->addAction(action);
    };
    for (const CitationStyle option : allCitationStyles())
        offer(citation::styleKey(option));
    QStringList recent = recentStyleKeys();
    if (isCslStyleKey(current) && !recent.contains(current))
        recent.prepend(current);
    if (!recent.isEmpty()) {
        styles->addSeparator();
        for (const QString &key : recent.mid(0, kRecentStyles))
            offer(key);
    }
    styles->addSeparator();
    QAction *more = styles->addAction(tr("More styles..."), this, [this, ids] { showStylesDialog(ids()); });
    more->setObjectName(QStringLiteral("moreCitationStyles"));
    menu->addSeparator();
    QAction *exporting = menu->addAction(count == 1 ? tr("Export citation...") : tr("Export %1 citations...").arg(count), this,
                                         [this, ids] { askAndExport(ids()); });
    exporting->setEnabled(count > 0 && !isExporting());
}

namespace {

void putOnClipboard(const QList<RichText> &entries)
{
    QStringList plain;
    QString html;
    for (const RichText &entry : entries) {
        plain << entry.plain();
        html += QStringLiteral("<p>") + entry.html() + QStringLiteral("</p>");
    }
    auto *data = new QMimeData;
    data->setText(plain.join(u'\n'));
    data->setHtml(QStringLiteral("<html><body>") + html + QStringLiteral("</body></html>"));
    QApplication::clipboard()->setMimeData(data);
}

// A reference is only as good as the details it's made from, and some of these have gaps.
QString lackingText(const QList<CitationItem> &items)
{
    const auto lacking = std::count_if(items.cbegin(), items.cend(), [](const CitationItem &item) { return lacksDetails(item); });
    if (lacking == 0)
        return {};
    return u' '
        + (items.size() == 1 ? CitationActions::tr("It's missing details such as its authors, year or journal.")
                             : CitationActions::tr("%1 of them are missing details such as their authors, year or journal.").arg(lacking))
        + u' ' + CitationActions::tr("Look up details online can fill them in.");
}

} // namespace

std::optional<CslLayout> CitationActions::cslToCopyIn()
{
    const QString key = styleKey();
    if (!isCslStyleKey(key))
        return std::nullopt;
    QString error;
    std::optional<CslLayout> layout = cslLayout(key, &error);
    if (!layout)
        emit message(tr("%1 can't be used, so Harvard is used instead: %2").arg(styleTitle(key), error));
    return layout;
}

void CitationActions::startCslCopy(const CslLayout &layout, const QList<CitationItem> &items, bool references)
{
    m_copyRequest = cslThread()->render(QStringLiteral("copy"), references ? CslThread::Kind::References : CslThread::Kind::Citation,
                                        layout, items, styleOptions());
    emit message(references ? (items.size() == 1 ? tr("Copying a reference in %1...").arg(layout.style.title)
                                                 : tr("Copying %1 references in %2...").arg(items.size()).arg(layout.style.title))
                            : tr("Copying a citation in %1...").arg(layout.style.title));
}

void CitationActions::onCslFinished(qint64 request, const QList<RichText> &entries, const QList<int> &, const QString &error)
{
    if (request != m_copyRequest)
        return;
    m_copyRequest = 0;
    if (!error.isEmpty()) {
        emit message(tr("Nothing was copied: %1").arg(error));
        emit copyFinished(false);
        return;
    }
    putOnClipboard(entries);
    emit message(m_copyText);
    emit copyFinished(true);
}

bool CitationActions::copyReferences(const QList<qint64> &ids)
{
    if (ids.isEmpty() || ids.size() > kMaxToCopy)
        return false;
    const QList<CitationItem> items = itemsFor(ids);
    if (items.isEmpty())
        return false;
    if (const std::optional<CslLayout> layout = cslToCopyIn()) {
        m_copyText = (items.size() == 1 ? tr("Copied a reference in %1.").arg(layout->style.title)
                                        : tr("Copied %1 references in %2.").arg(items.size()).arg(layout->style.title))
            + lackingText(items);
        startCslCopy(*layout, items, true);
        return true;
    }
    putOnClipboard(formatReferences(items, style(), styleOptions()).entries);
    const QString name = styleName(style());
    emit message((items.size() == 1 ? tr("Copied a reference in %1.").arg(name) : tr("Copied %1 references in %2.").arg(items.size()).arg(name))
                 + lackingText(items));
    return true;
}

bool CitationActions::copyCaseCitations(const QList<qint64> &ids, bool oscola)
{
    if (ids.isEmpty() || ids.size() > kMaxToCopy)
        return false;
    QList<CitationItem> items = itemsFor(ids);
    if (items.isEmpty())
        return false;
    QList<RichText> entries;
    for (CitationItem &item : items) {
        item.oscola = oscola;
        entries << caseCitation(item);
    }
    putOnClipboard(entries);
    const QString form = oscola ? tr("OSCOLA") : tr("the Bluebook");
    emit message(items.size() == 1 ? tr("Copied a citation in %1's form.").arg(form)
                                   : tr("Copied %1 citations in %2's form.").arg(items.size()).arg(form));
    return true;
}

bool CitationActions::copyInTextCitation(const QList<qint64> &ids)
{
    if (ids.isEmpty() || ids.size() > kMaxToCopy)
        return false;
    const QList<CitationItem> items = itemsFor(ids);
    if (items.isEmpty())
        return false;
    if (const std::optional<CslLayout> layout = cslToCopyIn()) {
        const bool note = readCslStyle(layout->xml).value_or(CslStyleInfo{}).isNote;
        m_copyText = note ? tr("Copied a note in %1.").arg(layout->style.title) : tr("Copied an in-text citation in %1.").arg(layout->style.title);
        startCslCopy(*layout, items, false);
        return true;
    }
    putOnClipboard({combinedInTextCitation(items, style(), styleOptions())});
    emit message(tr("Copied an in-text citation in %1.").arg(styleName(style())));
    return true;
}

CitationStylesDialog *CitationActions::showStylesDialog(const QList<qint64> &ids, QWidget *parent)
{
    // A modal window, such as Settings, blocks every window but its own, so the dialog has to be the one asking's.
    QWidget *owner = parent ? parent : m_window;
    if (m_stylesDialog && m_stylesDialog->parentWidget() != owner) {
        m_stylesDialog->close();
        m_stylesDialog = nullptr;
    }
    if (!m_stylesDialog) {
        m_stylesDialog = new CitationStylesDialog(m_session, *this, owner);
        m_stylesDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_stylesDialog->showPreviewOf(itemsFor(ids.mid(0, CitationStylesDialog::kMaxPreviewed)));
    m_stylesDialog->show();
    m_stylesDialog->raise();
    m_stylesDialog->activateWindow();
    return m_stylesDialog;
}

namespace {

// A name for a file from a collection's name, which can hold anything, or "citations" if it's left with nothing.
QString fileNameFrom(const QString &name)
{
    QString cleaned;
    for (const QChar c : name.simplified()) {
        if (c.isLetterOrNumber() || c == u' ' || c == u'-' || c == u'_' || c == u'(' || c == u')')
            cleaned += c;
    }
    cleaned = cleaned.trimmed().left(80).trimmed();
    return cleaned.isEmpty() ? QStringLiteral("citations") : cleaned;
}

} // namespace

void CitationActions::askAndExport(const QList<qint64> &ids, const QString &suggestedName)
{
    if (ids.isEmpty() || isExporting())
        return;
    QStringList filters;
    for (const ExportFormat format : {ExportFormat::BibTeX, ExportFormat::Ris, ExportFormat::CslJson})
        filters << exportFileFilter(format);
    QString chosenFilter = filters.first();
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QFileDialog::getSaveFileName(m_window, tr("Export citations"), QDir(folder).filePath(fileNameFrom(suggestedName) + QStringLiteral(".bib")),
                                                filters.join(QStringLiteral(";;")), &chosenFilter);
    if (path.isEmpty())
        return;
    ExportFormat format = ExportFormat::BibTeX;
    for (const ExportFormat option : {ExportFormat::BibTeX, ExportFormat::Ris, ExportFormat::CslJson}) {
        if (chosenFilter == exportFileFilter(option))
            format = option;
    }
    // The file's own ending decides, if it has a known one.
    const QString suffix = QFileInfo(path).suffix().toLower();
    for (const ExportFormat option : {ExportFormat::BibTeX, ExportFormat::Ris, ExportFormat::CslJson}) {
        if (suffix == exportFileSuffix(option))
            format = option;
    }
    if (suffix.isEmpty())
        path += u'.' + exportFileSuffix(format);
    startExport(ids, path, format);
}

void CitationActions::startExport(const QList<qint64> &ids, const QString &path, ExportFormat format)
{
    if (isExporting())
        return;
    const QString databasePath = m_session.paths().database;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;

    m_progress = new QProgressDialog(tr("Exporting citations..."), tr("Cancel"), 0, int(ids.size()), m_window);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(500);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    connect(m_progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });

    QPointer<CitationActions> self(this);
    m_thread = QThread::create([self, cancel, ids, path, format, databasePath] {
        bool ok = false;
        QString text;
        try {
            // A connection of its own, so a large library doesn't hold up the window.
            core::Database db(databasePath);
            QList<CitationItem> items;
            items.reserve(ids.size());
            constexpr qsizetype kChunk = 500;
            for (qsizetype start = 0; start < ids.size(); start += kChunk) {
                if (cancel->load())
                    break;
                items += citationItems(db, ids.mid(start, kChunk));
                const int done = int(qMin(ids.size(), start + kChunk));
                QMetaObject::invokeMethod(qApp, [self, done] {
                    if (self && self->m_progress)
                        self->m_progress->setValue(done);
                }, Qt::QueuedConnection);
            }
            if (cancel->load()) {
                text = tr("The export was cancelled, and nothing was written.");
            } else {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly)) {
                    text = tr("%1 couldn't be written: %2").arg(QDir::toNativeSeparators(path), file.errorString());
                } else {
                    file.write(exportCitations(items, format));
                    if (file.commit()) {
                        ok = true;
                        text = items.size() == 1 ? tr("Exported 1 citation to %1.").arg(QDir::toNativeSeparators(path))
                                                 : tr("Exported %1 citations to %2.").arg(items.size()).arg(QDir::toNativeSeparators(path));
                    } else {
                        text = tr("%1 couldn't be written: %2").arg(QDir::toNativeSeparators(path), file.errorString());
                    }
                }
            }
        } catch (const std::exception &error) {
            text = tr("The export failed: %1").arg(QString::fromUtf8(error.what()));
        }
        QMetaObject::invokeMethod(qApp, [self, ok, text] {
            if (!self)
                return;
            if (self->m_progress) {
                self->m_progress->deleteLater();
                self->m_progress = nullptr;
            }
            if (self->m_thread) {
                self->m_thread->wait();
                self->m_thread->deleteLater();
                self->m_thread = nullptr;
            }
            emit self->message(text);
            emit self->exportFinished(ok, text);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
}

} // namespace papertrail::ui
