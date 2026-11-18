#pragma once

#include "import/WorkerClient.h"

#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QSizeF>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QProgressDialog;
class QSpinBox;
class QThread;
class QWidget;

namespace papertrail::ui {

class LibrarySession;

// What kind of book a PDF copy is made of, which decides what can be chosen for it.
enum class PdfCopyKind {
    Reflowable, // EPUB, Kindle and FB2, laid out at a page size.
    Djvu,       // Pages as pictures, with words that can be searched.
    Fixed,      // XPS and CBZ, whose pages are kept as they are.
};

// Whether a format can have a PDF copy made of it, and what kind. Not for a PDF, or a book in a zip, which has its
// stitched PDF.
std::optional<PdfCopyKind> pdfCopyKindFor(const QString &format);

// Chooses how a PDF copy of a book is made: for a reflowable book, its page size, type size and margins; for a DjVu,
// how sharp its pages stay and whether its words can be searched.
class PdfCopyDialog : public QDialog {
    Q_OBJECT

public:
    // `fontSize` is the reader's, and `readerPage` the page it lays books out on, in points.
    PdfCopyDialog(PdfCopyKind kind, double fontSize, const QSizeF &readerPage, QWidget *parent = nullptr);

    import::WorkerClient::PdfCopySettings settings() const;

    QComboBox *pageSizes() const { return m_pageSize; }
    QSpinBox *fontSize() const { return m_fontSize; }
    QComboBox *margins() const { return m_margins; }
    QComboBox *qualities() const { return m_quality; }
    QCheckBox *searchable() const { return m_searchable; }

private:
    PdfCopyKind m_kind;
    QComboBox *m_pageSize = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QComboBox *m_margins = nullptr;
    QComboBox *m_quality = nullptr;
    QCheckBox *m_searchable = nullptr;
};

// Writes one PDF copy through the worker, on a thread of its own. Cancelling stops the worker, and the part of the
// copy it was writing is removed, so nothing half-written is left behind.
class PdfCopyJob : public QObject {
    Q_OBJECT

public:
    struct Source {
        QString path;
        QString convertedPath; // Where a Kindle book MuPDF can't read is converted to.
        QString ocrFolder;     // Where words recognised on it are kept, if anywhere.
    };
    PdfCopyJob(const QString &workerPath, const Source &source, const QString &out,
               const import::WorkerClient::PdfCopySettings &settings, QObject *parent = nullptr);
    ~PdfCopyJob() override;

    void start();
    void cancel();
    bool isRunning() const { return m_thread != nullptr; }
    QString out() const { return m_out; }

signals:
    void progress(int done, int of);
    // `pages` written, of which `unreadable` were left blank. `error` says why it failed.
    void finished(bool ok, bool cancelled, const QString &error, int pages, int unreadable);

private:
    QString m_workerPath;
    Source m_source;
    QString m_out;
    import::WorkerClient::PdfCopySettings m_settings;
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};

// Save a PDF copy..., from the library's menu and the reader's: asks how and where, then writes it with a progress
// dialog that can stop it. The copy is a new file wherever the user chooses; the book itself is never changed.
class PdfCopyActions : public QObject {
    Q_OBJECT

public:
    PdfCopyActions(LibrarySession &session, QWidget *window);
    ~PdfCopyActions() override;

    // The file of a publication a copy would be made of, or nothing if it has none but PDFs.
    std::optional<qint64> fileFor(qint64 publicationId);
    // Asks how and where, then writes it. Of `fileId`, or else of the publication's first book that isn't a PDF.
    void askAndSave(qint64 publicationId, std::optional<qint64> fileId = std::nullopt);
    // Writes it without asking. Returns false if it can't start.
    bool save(qint64 fileId, const QString &out, const import::WorkerClient::PdfCopySettings &settings);
    bool isBusy() const { return m_job != nullptr; }
    PdfCopyJob *job() const { return m_job; }

signals:
    void finished(bool ok, const QString &message);

private:
    LibrarySession &m_session;
    QWidget *m_window;
    QPointer<PdfCopyJob> m_job;
    QPointer<QProgressDialog> m_progress;
};

} // namespace papertrail::ui
