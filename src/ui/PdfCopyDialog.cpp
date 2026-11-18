#include "ui/PdfCopyDialog.h"

#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "import/ImportService.h"
#include "ui/LibrarySession.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QThread>
#include <QVBoxLayout>

#include <cmath>

namespace papertrail::ui {

using namespace core;

namespace {

constexpr int kSilenceMs = 10 * 60 * 1000; // The longest the worker may go without a word, such as while saving.

struct PageChoice {
    const char *name;
    double width;
    double height;
};

const PageChoice kPageSizes[] = {
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "A4"), 595, 842},
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "US Letter"), 612, 792},
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "A5"), 420, 595},
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "6 × 9 in"), 432, 648},
};

struct MarginChoice {
    const char *name;
    double points;
};

const MarginChoice kMargins[] = {
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "Narrow"), 36},
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "Normal"), 54},
    {QT_TRANSLATE_NOOP("PdfCopyDialog", "Wide"), 90},
};

// A name for a file from a title, which can hold anything, or "Book" if it's left with nothing.
QString fileNameFrom(const QString &title)
{
    QString cleaned;
    for (const QChar c : title.simplified()) {
        if (c.isLetterOrNumber() || c == u' ' || c == u'-' || c == u'_' || c == u'(' || c == u')' || c == u',' || c == u'\'')
            cleaned += c;
        else if (c == u':' || c == u'/' || c == u'\\')
            cleaned += QStringLiteral(" - ");
    }
    cleaned = cleaned.simplified().left(120).trimmed();
    return cleaned.isEmpty() ? QStringLiteral("Book") : cleaned;
}

Qt::CaseSensitivity pathCase()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool samePath(const QString &a, const QString &b)
{
    const QFileInfo first(a);
    const QFileInfo second(b);
    if (QDir::cleanPath(first.absoluteFilePath()).compare(QDir::cleanPath(second.absoluteFilePath()), pathCase()) == 0)
        return true;
    return first.exists() && second.exists() && first.canonicalFilePath() == second.canonicalFilePath();
}

// Whether `path` is inside the folder `root`.
bool isInside(const QString &path, const QString &root)
{
    QString folder = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const QString file = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!folder.endsWith(u'/'))
        folder += u'/';
    return file.startsWith(folder, pathCase());
}

void tell(QWidget *window, QMessageBox::Icon icon, const QString &text)
{
    QMessageBox box(icon, PdfCopyActions::tr("Save a PDF copy"), text, QMessageBox::Ok, window);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

} // namespace

std::optional<PdfCopyKind> pdfCopyKindFor(const QString &format)
{
    const QString f = format.toLower();
    if (f == QLatin1StringView("epub") || f == QLatin1StringView("mobi") || f == QLatin1StringView("azw")
        || f == QLatin1StringView("azw3") || f == QLatin1StringView("fb2")) {
        return PdfCopyKind::Reflowable;
    }
    if (f == QLatin1StringView("djvu") || f == QLatin1StringView("djv"))
        return PdfCopyKind::Djvu;
    if (f == QLatin1StringView("xps") || f == QLatin1StringView("oxps") || f == QLatin1StringView("cbz"))
        return PdfCopyKind::Fixed;
    return std::nullopt;
}

PdfCopyDialog::PdfCopyDialog(PdfCopyKind kind, double fontSize, const QSizeF &readerPage, QWidget *parent)
    : QDialog(parent), m_kind(kind)
{
    setWindowTitle(tr("Save a PDF copy"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    if (kind == PdfCopyKind::Reflowable) {
        auto *about = new QLabel(tr("The book is laid out afresh on pages of this size, and its text stays text."), this);
        about->setWordWrap(true);
        layout->insertWidget(0, about);
        m_pageSize = new QComboBox(this);
        m_pageSize->setObjectName(QStringLiteral("pageSize"));
        for (const PageChoice &choice : kPageSizes)
            m_pageSize->addItem(tr(choice.name), QSizeF(choice.width, choice.height));
        m_pageSize->addItem(tr("As the reader lays it out"), readerPage);
        form->addRow(tr("Page size:"), m_pageSize);

        m_fontSize = new QSpinBox(this);
        m_fontSize->setObjectName(QStringLiteral("fontSize"));
        m_fontSize->setRange(8, 32);
        m_fontSize->setSuffix(tr(" pt"));
        m_fontSize->setValue(qBound(8, int(std::lround(fontSize)), 32));
        form->addRow(tr("Type size:"), m_fontSize);

        m_margins = new QComboBox(this);
        m_margins->setObjectName(QStringLiteral("margins"));
        for (const MarginChoice &choice : kMargins)
            m_margins->addItem(tr(choice.name), choice.points);
        m_margins->setCurrentIndex(1);
        form->addRow(tr("Margins:"), m_margins);
    } else if (kind == PdfCopyKind::Djvu) {
        auto *about = new QLabel(tr("Each page is kept as a picture of it."), this);
        about->setWordWrap(true);
        layout->insertWidget(0, about);
        m_quality = new QComboBox(this);
        m_quality->setObjectName(QStringLiteral("quality"));
        m_quality->addItem(tr("300 dpi, or the file's own if it's lower"), 300);
        m_quality->addItem(tr("The file's own resolution"), 0);
        form->addRow(tr("Quality:"), m_quality);
        m_searchable = new QCheckBox(tr("Keep its words searchable"), this);
        m_searchable->setObjectName(QStringLiteral("searchable"));
        m_searchable->setChecked(true);
        m_searchable->setToolTip(tr("Its hidden text, or words recognised on its pages, go over the pictures as text that can't be seen."));
        form->addRow(QString(), m_searchable);
    } else {
        auto *about = new QLabel(tr("Its pages are kept as they are."), this);
        about->setWordWrap(true);
        layout->insertWidget(0, about);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Choose where to save..."));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

import::WorkerClient::PdfCopySettings PdfCopyDialog::settings() const
{
    import::WorkerClient::PdfCopySettings settings;
    if (m_pageSize) {
        const QSizeF page = m_pageSize->currentData().toSizeF();
        if (page.width() > 0 && page.height() > 0) {
            settings.pageWidth = page.width();
            settings.pageHeight = page.height();
        }
    }
    if (m_fontSize)
        settings.fontSize = m_fontSize->value();
    if (m_margins)
        settings.margin = m_margins->currentData().toDouble();
    if (m_quality)
        settings.dpi = m_quality->currentData().toInt();
    if (m_searchable)
        settings.searchable = m_searchable->isChecked();
    return settings;
}

PdfCopyJob::PdfCopyJob(const QString &workerPath, const Source &source, const QString &out,
                       const import::WorkerClient::PdfCopySettings &settings, QObject *parent)
    : QObject(parent)
    , m_workerPath(workerPath)
    , m_source(source)
    , m_out(out)
    , m_settings(settings)
    , m_cancelled(std::make_shared<std::atomic<bool>>(false))
{
}

PdfCopyJob::~PdfCopyJob()
{
    if (m_thread) {
        m_cancelled->store(true);
        m_thread->wait();
        delete m_thread;
        QFile::remove(m_out + QStringLiteral(".part"));
    }
}

void PdfCopyJob::start()
{
    if (m_thread)
        return;
    auto result = std::make_shared<import::WorkerClient::PdfCopy>();
    const auto cancelled = m_cancelled;
    const QString workerPath = m_workerPath;
    const Source source = m_source;
    const QString out = m_out;
    const auto settings = m_settings;
    // The job outlives its thread, since it waits for it, so its signals can be sent from there.
    m_thread = QThread::create([this, workerPath, source, out, settings, cancelled, result] {
        import::WorkerClient worker(workerPath, [cancelled] { return cancelled->load(); });
        *result = worker.pdfCopy(source.path, source.convertedPath, out, settings, kSilenceMs,
                                 [this](int done, int of) { emit progress(done, of); }, source.ocrFolder);
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        m_thread->deleteLater();
        m_thread = nullptr;
        const bool ok = result->failure == import::WorkerFailure::None;
        // A worker stopped part way, by the user or by a crash, can leave a part written.
        if (!ok)
            QFile::remove(m_out + QStringLiteral(".part"));
        emit finished(ok, result->failure == import::WorkerFailure::Cancelled, result->error, result->pages, result->unreadablePages);
    });
    m_thread->start();
}

void PdfCopyJob::cancel()
{
    m_cancelled->store(true);
}

PdfCopyActions::PdfCopyActions(LibrarySession &session, QWidget *window)
    : QObject(window)
    , m_session(session)
    , m_window(window)
{
}

PdfCopyActions::~PdfCopyActions()
{
    delete m_job;
}

std::optional<qint64> PdfCopyActions::fileFor(qint64 publicationId)
{
    const std::optional<Publication> publication = m_session.publications().get(publicationId);
    if (!publication)
        return std::nullopt;
    for (const FileRecord &file : m_session.files().filesFor(publication->parentId.value_or(publicationId))) {
        if (pdfCopyKindFor(file.format) && !file.sha256.isEmpty() && file.status == FileStatus::Ok)
            return file.id;
    }
    return std::nullopt;
}

void PdfCopyActions::askAndSave(qint64 publicationId, std::optional<qint64> fileId)
{
    if (m_job) {
        tell(m_window, QMessageBox::Information, tr("A PDF copy is already being saved. Wait for it to finish, or stop it."));
        return;
    }
    std::optional<FileRecord> file;
    QString original;
    QString title;
    try {
        if (!fileId)
            fileId = fileFor(publicationId);
        if (fileId)
            file = m_session.files().get(*fileId);
        if (file)
            original = m_session.files().absolutePath(file->id);
        if (const std::optional<Publication> publication = m_session.publications().get(publicationId))
            title = publication->title;
    } catch (const std::exception &) {
        file.reset();
    }
    const std::optional<PdfCopyKind> kind = file ? pdfCopyKindFor(file->format) : std::nullopt;
    if (!file || !kind) {
        tell(m_window, QMessageBox::Information, tr("This publication has no book in another format to make a PDF of."));
        return;
    }
    if (!QFileInfo::exists(original)) {
        tell(m_window, QMessageBox::Warning, tr("The book's file can't be found right now."));
        return;
    }

    const double fontSize = m_session.settings().value(QStringLiteral("reader.font_size")).toDouble(12);
    PdfCopyDialog dialog(*kind, fontSize, QSizeF(520, 720), m_window);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto settings = dialog.settings();

    // Somewhere outside the library by default: a copy in one of its folders would be added to it.
    const QString name = fileNameFrom(title.isEmpty() ? QFileInfo(original).completeBaseName() : title);
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(name + QStringLiteral(".pdf"));
    QString out = QFileDialog::getSaveFileName(m_window, tr("Save a PDF copy"), suggested, tr("PDF (*.pdf)"));
    if (out.isEmpty())
        return;
    if (samePath(out, original)) {
        tell(m_window, QMessageBox::Warning, tr("That's the book itself, which Papertrail never changes. Choose another name or folder."));
        return;
    }
    QList<LibraryRoot> roots;
    try {
        roots = m_session.files().roots();
    } catch (const std::exception &) {
    }
    for (const LibraryRoot &root : std::as_const(roots)) {
        if (!isInside(out, root.path))
            continue;
        QMessageBox box(QMessageBox::Question, tr("Save a PDF copy"),
                        tr("That folder is one of the library's, so the copy will be added to this book as another format "
                           "the next time it's scanned."),
                        QMessageBox::NoButton, m_window);
        box.setTextFormat(Qt::PlainText);
        box.addButton(tr("Save it there"), QMessageBox::AcceptRole);
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == cancel)
            return;
        break;
    }
    if (!save(file->id, out, settings))
        return;

    m_progress = new QProgressDialog(tr("Saving a PDF copy..."), tr("Stop"), 0, 0, m_window);
    m_progress->setWindowTitle(tr("Save a PDF copy"));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(500);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    connect(m_progress, &QProgressDialog::canceled, m_job, &PdfCopyJob::cancel);
    connect(m_job, &PdfCopyJob::progress, m_progress, [this](int done, int of) {
        if (!m_progress)
            return;
        m_progress->setMaximum(of);
        m_progress->setValue(qMin(done, of));
        m_progress->setLabelText(tr("Saving a PDF copy: page %1 of %2...").arg(done).arg(of));
    });
}

bool PdfCopyActions::save(qint64 fileId, const QString &out, const import::WorkerClient::PdfCopySettings &settings)
{
    if (m_job)
        return false;
    PdfCopyJob::Source source;
    try {
        const std::optional<FileRecord> file = m_session.files().get(fileId);
        if (!file || !pdfCopyKindFor(file->format))
            return false;
        source.path = m_session.files().absolutePath(fileId);
        source.convertedPath = import::convertedPathFor(m_session.paths().cacheDirectory, file->sha256, source.path);
        if (pdfCopyKindFor(file->format) == PdfCopyKind::Djvu)
            source.ocrFolder = import::recognisedFolderFor(m_session.paths().cacheDirectory, file->sha256);
    } catch (const std::exception &) {
        return false;
    }
    if (samePath(out, source.path))
        return false;

    m_job = new PdfCopyJob(m_session.paths().workerPath, source, out, settings, this);
    connect(m_job, &PdfCopyJob::finished, this, [this](bool ok, bool cancelled, const QString &error, int pages, int unreadable) {
        const QString where = QDir::toNativeSeparators(m_job->out());
        m_job->deleteLater();
        m_job = nullptr;
        if (m_progress) {
            m_progress->deleteLater();
            m_progress = nullptr;
        }
        QString message;
        if (cancelled)
            message = tr("Stopped saving the PDF copy.");
        else if (!ok)
            message = tr("The PDF copy couldn't be saved: %1").arg(error);
        else if (unreadable > 0)
            message = tr("Saved a PDF copy of %n page(s) as %1, though %2 of them couldn't be read and are blank.", nullptr, pages)
                          .arg(where)
                          .arg(unreadable);
        else
            message = tr("Saved a PDF copy of %n page(s) as %1.", nullptr, pages).arg(where);
        emit finished(ok || cancelled, message);
    });
    m_job->start();
    return true;
}

} // namespace papertrail::ui
