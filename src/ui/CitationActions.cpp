#include "ui/CitationActions.h"

#include "core/db/Database.h"
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

CitationStyle CitationActions::style() const
{
    return styleFromKey(m_session.settings().value(QStringLiteral("citation.style")).toString()).value_or(CitationStyle::Harvard);
}

void CitationActions::setStyle(CitationStyle chosen)
{
    try {
        m_session.settings().setValue(QStringLiteral("citation.style"), styleKey(chosen));
    } catch (const std::exception &error) {
        emit message(tr("The citation style couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
    }
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
    const QString name = styleName(style());
    QAction *references = menu->addAction(count == 1 ? tr("Copy reference (%1)").arg(name)
                                                     : tr("Copy %1 references (%2)").arg(count).arg(name),
                                          this, [this, ids] { copyReferences(ids()); });
    QAction *inText = menu->addAction(tr("Copy in-text citation (%1)").arg(name), this, [this, ids] { copyInTextCitation(ids()); });
    references->setEnabled(canCopy);
    inText->setEnabled(canCopy);
    if (count > kMaxToCopy)
        references->setToolTip(tr("Too many to copy at once. Export them to a file instead."));

    QMenu *styles = menu->addMenu(tr("Style"));
    auto *group = new QActionGroup(styles);
    group->setExclusive(true);
    for (const CitationStyle option : allCitationStyles()) {
        QAction *action = styles->addAction(styleName(option), this, [this, option] { setStyle(option); });
        action->setCheckable(true);
        action->setChecked(option == style());
        group->addAction(action);
    }
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

} // namespace

bool CitationActions::copyReferences(const QList<qint64> &ids)
{
    if (ids.isEmpty() || ids.size() > kMaxToCopy)
        return false;
    const QList<CitationItem> items = itemsFor(ids);
    if (items.isEmpty())
        return false;
    putOnClipboard(formatReferences(items, style(), styleOptions()).entries);
    QString text = items.size() == 1 ? tr("Copied a reference in %1.").arg(styleName(style()))
                                     : tr("Copied %1 references in %2.").arg(items.size()).arg(styleName(style()));
    // A reference is only as good as the details it's made from, and some of these have gaps.
    const auto lacking = std::count_if(items.cbegin(), items.cend(), [](const CitationItem &item) { return lacksDetails(item); });
    if (lacking > 0) {
        text += u' '
            + (items.size() == 1 ? tr("It's missing details such as its authors, year or journal.")
                                 : tr("%1 of them are missing details such as their authors, year or journal.").arg(lacking))
            + u' ' + tr("Look up details online can fill them in.");
    }
    emit message(text);
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
    putOnClipboard({combinedInTextCitation(items, style(), styleOptions())});
    emit message(tr("Copied an in-text citation in %1.").arg(styleName(style())));
    return true;
}

void CitationActions::askAndExport(const QList<qint64> &ids)
{
    if (ids.isEmpty() || isExporting())
        return;
    QStringList filters;
    for (const ExportFormat format : {ExportFormat::BibTeX, ExportFormat::Ris, ExportFormat::CslJson})
        filters << exportFileFilter(format);
    QString chosenFilter = filters.first();
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QFileDialog::getSaveFileName(m_window, tr("Export citations"), QDir(folder).filePath(QStringLiteral("citations.bib")),
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
