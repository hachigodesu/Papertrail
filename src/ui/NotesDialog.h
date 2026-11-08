#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace papertrail::ui {

class LibrarySession;

// Every highlight and note in the library in one list, searchable, with each opening its document at that place.
class NotesDialog : public QDialog {
    Q_OBJECT

public:
    explicit NotesDialog(LibrarySession &session, QWidget *parent = nullptr);

    QLineEdit *searchBox() const { return m_search; }
    QTreeWidget *list() const { return m_list; }
    QLabel *countLabel() const { return m_count; }

    // Shows what the search box asks for now, rather than after the short pause that typing allows.
    void reload();

    // How many are listed at most. More than this asks for a search to narrow them.
    static constexpr int kShownAtMost = 2000;

signals:
    // The user wants to see this highlight or note in its document.
    void openRequested(qint64 annotationId);

private:
    void openChosen();

    LibrarySession &m_session;
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_count = nullptr;
    QPushButton *m_open = nullptr;
    QTimer *m_typing = nullptr;
};

} // namespace papertrail::ui
