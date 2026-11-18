#pragma once

#include "citation/CitationItem.h"
#include "citation/CslStyles.h"
#include "citation/Formatting.h"

#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

class CitationActions;
class LibrarySession;

// Every citation style there is: the hand-written ones, the CSL styles that come with Papertrail and any added to
// this library from a file, with a preview of the publications chosen, or of made-up ones.
class CitationStylesDialog : public QDialog {
    Q_OBJECT

public:
    static constexpr int kMaxPreviewed = 5;

    CitationStylesDialog(LibrarySession &session, CitationActions &citations, QWidget *parent = nullptr);

    // What the preview shows. Made-up publications, if there are none.
    void showPreviewOf(const QList<citation::CitationItem> &items);

    // Checks a style from a file and, if it can be used, adds it to the library. `addFinished` follows.
    void addStyleFile(const QString &path);
    // The style chosen in the list, by its key.
    QString chosenKey() const;
    void choose(const QString &key);
    void removeChosen();
    void useChosen();

    QTreeWidget *styleList() const { return m_list; }
    QTextBrowser *preview() const { return m_preview; }
    QLabel *status() const { return m_status; }
    QLineEdit *search() const { return m_search; }

signals:
    void addFinished(bool ok, const QString &message);
    void previewShown();

private:
    void fill(const QString &keep = QString());
    void filter();
    void updatePreview();
    void updateButtons();
    void onRendered(qint64 request, const QList<citation::RichText> &entries, const QList<int> &order, const QString &error);
    void showPreviewText();

    LibrarySession &m_session;
    CitationActions &m_citations;
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_list = nullptr;
    QTextBrowser *m_preview = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_use = nullptr;
    QList<citation::CitationItem> m_items;

    qint64 m_referencesRequest = 0;
    qint64 m_citationRequest = 0;
    qint64 m_checkRequest = 0;
    QList<citation::RichText> m_previewReferences;
    QString m_previewCitation;
    QString m_previewError;
    // A style from a file waiting to be checked before it's added.
    citation::CslStyleInfo m_checking;
    QByteArray m_checkingXml;
};

} // namespace papertrail::ui
