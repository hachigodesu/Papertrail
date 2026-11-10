#pragma once

#include "metadata/RelatedFinder.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

class LibrarySession;
class LookupThread;

// The papers a publication cites, and the most cited of those citing it, from OpenAlex. Those already in the library
// are marked, and others can be added without a file. It asks OpenAlex only when opened with nothing recent kept, or
// when Refresh is pressed, and only through the lookup thread, which refuses while lookups are offline.
class RelatedPapersDialog : public QDialog {
    Q_OBJECT

public:
    RelatedPapersDialog(LibrarySession &session, LookupThread *lookups, qint64 publicationId, QWidget *parent = nullptr);

    qint64 publicationId() const { return m_publicationId; }
    QTreeWidget *references() const { return m_references; }
    QTreeWidget *citedBy() const { return m_citedBy; }
    QLabel *status() const { return m_status; }
    QPushButton *addButton() const { return m_add; }
    QPushButton *refreshButton() const { return m_refresh; }

    // Shows what's kept, and asks for it again if there's nothing recent.
    void load(bool refresh = false);

signals:
    // Publications were added to the library.
    void libraryChanged();
    // To see this publication in the library.
    void showRequested(qint64 publicationId);

private:
    void fill();
    void fillTree(QTreeWidget *tree, const QList<metadata::RelatedWork> &works);
    void onFound(qint64 publicationId);
    void onFailed(qint64 publicationId, int problem, const QString &detail);
    QList<QTreeWidgetItem *> chosen() const;
    const metadata::RelatedWork *workFor(QTreeWidgetItem *item) const;
    void updateButtons();
    void addChosen();
    void copyDois();
    void openChosen();
    void showChosen();

    LibrarySession &m_session;
    LookupThread *m_lookups;
    qint64 m_publicationId;
    metadata::RelatedPapers m_papers;
    bool m_asking = false;

    QLabel *m_heading = nullptr;
    QLabel *m_status = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTreeWidget *m_references = nullptr;
    QTreeWidget *m_citedBy = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_show = nullptr;
    QPushButton *m_refresh = nullptr;
};

} // namespace papertrail::ui
