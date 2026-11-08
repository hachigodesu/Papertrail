#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

#include <optional>

class QThread;
class QWidget;

namespace papertrail::core {
class Database;
}

namespace papertrail::ui {

class LibrarySession;

// The marks to write into a copy of a file: every highlight, underline and note made on its contents, under any
// publication, on fixed pages. A place in reflowable text has no page to write it on, so those are left out.
QJsonArray marksForContents(core::Database &db, const QString &contentHash, int *count = nullptr);

// The PDF of a publication, or of the issue an article's in, with the most highlights, for saving a copy of.
std::optional<qint64> markedPdfFor(LibrarySession &session, qint64 publicationId);

// Saves a copy of a PDF with its highlights and notes written in, for reading in other apps. The copy is a new file
// wherever the user chooses; the original is never changed. The worker writes it, on a thread of its own.
class MarkedCopyActions : public QObject {
    Q_OBJECT

public:
    MarkedCopyActions(LibrarySession &session, QWidget *window);
    ~MarkedCopyActions() override;

    // Asks where, then writes it. Says so if there's nothing to write.
    void askAndSave(qint64 publicationId);
    // Writes it without asking. Returns false if it can't start, such as with no highlights to write.
    bool save(qint64 fileId, const QString &out);
    bool isBusy() const { return m_thread != nullptr; }

signals:
    void finished(bool ok, const QString &message);

private:
    LibrarySession &m_session;
    QWidget *m_window;
    QThread *m_thread = nullptr;
};

} // namespace papertrail::ui
