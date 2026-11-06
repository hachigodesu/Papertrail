#pragma once

#include "core/model/Publication.h"

#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QCompleter;

namespace papertrail::ui {

class CoverCache;
class LibrarySession;

// Everything known about the selected publication, with its tags, read status and notes editable
// in place. With several selected, it offers changes to all of them.
class DetailsPanel : public QWidget {
    Q_OBJECT

public:
    DetailsPanel(LibrarySession &session, CoverCache &covers, QWidget *parent = nullptr);

    void showPublication(qint64 id);
    void showSelectionCount(qint64 count);
    void showNothing();
    void reload(); // Reads the shown publication again, after a change elsewhere.
    void saveNotes(); // Saves any notes still waiting to be saved.

    std::optional<qint64> currentId() const { return m_id; }

    // Adds a tag to the shown publication, creating it if needed. A path such as
    // "Engineering > FEM" creates each level.
    void addTag(const QString &path);
    void removeTag(qint64 tagId);

    // For tests.
    QLabel *titleLabel() const { return m_title; }
    QLabel *authorsLabel() const { return m_authors; }
    QFormLayout *facts() const { return m_facts; }
    QComboBox *readStatusBox() const { return m_readStatus; }
    QCheckBox *preprintBox() const { return m_preprint; }
    QPlainTextEdit *notesEdit() const { return m_notes; }
    QPushButton *editButton() const { return m_edit; }

signals:
    void libraryEdited();
    void editRequested(qint64 id);
    void openRequested(qint64 id);
    void showInFolderRequested(qint64 id);

    // Changes for everything selected, from the several-selected page.
    void bulkTagRequested(const QString &path);
    void bulkReadStatusRequested(core::ReadStatus status);
    void bulkMarkCheckedRequested();

private:
    void buildSinglePage();
    void buildManyPage();
    void addFact(const QString &label, const QString &value);
    void addLink(const QString &label, const QString &text, const QUrl &url);
    void refreshTagCompleter(QLineEdit *edit);
    void changePublication(const std::function<void(core::Publication &)> &change);
    void setMarkedAsPreprint(bool marked);
    void updateCover();

    LibrarySession &m_session;
    CoverCache &m_covers;
    std::optional<qint64> m_id;
    QString m_coverSha;
    bool m_loading = false;
    bool m_notesDirty = false;

    QStackedWidget *m_pages = nullptr;
    QLabel *m_cover = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_authors = nullptr;
    QWidget *m_review = nullptr;
    QLabel *m_reviewReason = nullptr;
    QFormLayout *m_facts = nullptr;
    QComboBox *m_readStatus = nullptr;
    QComboBox *m_rating = nullptr;
    QVBoxLayout *m_tagRows = nullptr;
    QLineEdit *m_tagEdit = nullptr;
    QCheckBox *m_preprint = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QTimer *m_notesTimer = nullptr;
    QVBoxLayout *m_fileRows = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_showInFolder = nullptr;
    QPushButton *m_edit = nullptr;

    QLabel *m_manyCount = nullptr;
    QLineEdit *m_manyTagEdit = nullptr;
};

} // namespace papertrail::ui
