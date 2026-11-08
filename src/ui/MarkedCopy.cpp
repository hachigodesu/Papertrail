#include "ui/MarkedCopy.h"

#include "core/repo/AnnotationRepository.h"
#include "core/repo/FileRepository.h"
#include "import/WorkerClient.h"
#include "ui/LibrarySession.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QMessageBox>
#include <QStandardPaths>
#include <QThread>

namespace papertrail::ui {

using namespace core;

QJsonArray marksForContents(Database &db, const QString &contentHash, int *count)
{
    QJsonArray marks;
    int annotations = 0;
    for (const Annotation &annotation : AnnotationRepository(db).forContents(contentHash)) {
        bool any = false;
        for (const AnnotationPart &part : annotation.parts) {
            if (part.page < 0 || part.rects.isEmpty())
                continue; // A place in reflowable text, with no page to put it on.
            QJsonArray rects;
            for (const QRectF &rect : part.rects)
                rects.append(QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()});
            const QString kind = annotation.type == AnnotationType::Underline ? QStringLiteral("underline")
                : annotation.type == AnnotationType::Note                    ? QStringLiteral("note")
                                                                              : QStringLiteral("highlight");
            // A note goes with the first part only, so it's written once.
            marks.append(QJsonObject{{QStringLiteral("page"), part.page},
                                     {QStringLiteral("kind"), kind},
                                     {QStringLiteral("colour"), annotation.colour},
                                     {QStringLiteral("note"), any ? QString() : annotation.note},
                                     {QStringLiteral("rects"), rects}});
            any = true;
        }
        annotations += any ? 1 : 0;
    }
    if (count)
        *count = annotations;
    return marks;
}

std::optional<qint64> markedPdfFor(LibrarySession &session, qint64 publicationId)
{
    const std::optional<Publication> publication = session.publications().get(publicationId);
    if (!publication)
        return std::nullopt;
    std::optional<qint64> best;
    int most = 0;
    for (const FileRecord &file : session.files().filesFor(publication->parentId.value_or(publicationId))) {
        if (file.format != QLatin1StringView("pdf") || file.sha256.isEmpty() || file.status != FileStatus::Ok)
            continue;
        int count = 0;
        marksForContents(session.database(), file.sha256, &count);
        if (count > most) {
            most = count;
            best = file.id;
        }
    }
    return best;
}

MarkedCopyActions::MarkedCopyActions(LibrarySession &session, QWidget *window)
    : QObject(window)
    , m_session(session)
    , m_window(window)
{
}

MarkedCopyActions::~MarkedCopyActions()
{
    if (m_thread)
        m_thread->wait();
}

void MarkedCopyActions::askAndSave(qint64 publicationId)
{
    std::optional<qint64> fileId;
    try {
        fileId = markedPdfFor(m_session, publicationId);
    } catch (const std::exception &) {
    }
    if (!fileId) {
        QMessageBox box(QMessageBox::Information, tr("Save a copy with highlights"),
                        tr("There are no highlights or notes on a page of this publication's PDF to save in a copy."),
                        QMessageBox::Ok, m_window);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    const QString original = m_session.files().absolutePath(*fileId);
    // Somewhere outside the library by default: a copy in one of its folders would be added as a publication of its own.
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .filePath(tr("%1 (highlighted).pdf").arg(QFileInfo(original).completeBaseName()));
    const QString out = QFileDialog::getSaveFileName(m_window, tr("Save a copy with highlights"), suggested, tr("PDF (*.pdf)"));
    if (out.isEmpty())
        return;
    if (QFileInfo(out).absoluteFilePath() == QFileInfo(original).absoluteFilePath()) {
        QMessageBox box(QMessageBox::Warning, tr("Save a copy with highlights"),
                        tr("That's the original file, which Papertrail never changes. Choose another name or folder."),
                        QMessageBox::Ok, m_window);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    save(*fileId, out);
}

bool MarkedCopyActions::save(qint64 fileId, const QString &out)
{
    if (m_thread)
        return false;
    QString path;
    QJsonArray marks;
    int count = 0;
    try {
        const std::optional<FileRecord> file = m_session.files().get(fileId);
        if (!file)
            return false;
        path = m_session.files().absolutePath(fileId);
        marks = marksForContents(m_session.database(), file->sha256, &count);
    } catch (const std::exception &) {
        return false;
    }
    if (marks.isEmpty())
        return false;

    const QString workerPath = m_session.paths().workerPath;
    auto result = std::make_shared<import::WorkerClient::MarkedCopy>();
    m_thread = QThread::create([workerPath, path, out, marks, result] {
        import::WorkerClient worker(workerPath);
        *result = worker.markedCopy(path, out, marks, 5 * 60 * 1000);
    });
    connect(m_thread, &QThread::finished, this, [this, result, out, count] {
        m_thread->deleteLater();
        m_thread = nullptr;
        const bool ok = result->failure == import::WorkerFailure::None;
        const QString where = QDir::toNativeSeparators(out);
        QString message;
        if (!ok)
            message = tr("The copy couldn't be saved: %1").arg(result->error);
        else if (count == 1)
            message = tr("Saved a copy with 1 highlight or note as %1.").arg(where);
        else
            message = tr("Saved a copy with %1 highlights and notes as %2.").arg(count).arg(where);
        emit finished(ok, message);
    });
    m_thread->start();
    return true;
}

} // namespace papertrail::ui
