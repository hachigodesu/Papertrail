#pragma once

#include "import/Stitching.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QThread;

namespace papertrail::ui {

class LibrarySession;

// The order of a book's parts, for a book downloaded as a zip of PDFs. It reads what each part shows through the worker,
// on a thread, so the window never waits, and lists them in the order kept, with why each is where it is. Parts can be
// dragged or moved, and left out or put back. Stitch accepts it, and the window applies the order chosen.
class StitchDialog : public QDialog {
    Q_OBJECT

public:
    StitchDialog(LibrarySession &session, const QString &zipSha256, const QString &zipPath, const QString &title,
                 QWidget *parent = nullptr);
    ~StitchDialog() override;

    QString zipSha256() const { return m_sha256; }
    // The order chosen, once Stitch is pressed.
    QList<import::OrderedPart> chosenOrder() const;
    // Whether anything differs from the order kept.
    bool changed() const;

    // For tests.
    QListWidget *list() const { return m_list; }
    QPushButton *button(const QString &name) const;
    bool isReading() const { return m_thread != nullptr; }

signals:
    void partsRead();

private:
    void onRead(const import::StitchPlan &plan, const QString &error);
    void fill(const QList<import::PlacedPart> &parts);
    void moveCurrent(int by);
    void workOutAgain();
    void updateButtons();
    void stitch();

    LibrarySession &m_session;
    QString m_sha256;
    QString m_zipPath;
    QString m_coverFolder;
    QList<import::OrderedPart> m_kept;
    import::StitchPlan m_workedOut;
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancelled = std::make_shared<std::atomic<bool>>(false);
    QLabel *m_summary = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QPushButton *m_again = nullptr;
    QPushButton *m_stitch = nullptr;
};

} // namespace papertrail::ui
