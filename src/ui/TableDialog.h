#pragma once

#include "core/text/TableCells.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QTableWidget;

namespace papertrail::ui {

// A table read from an area of a page, shown as cells so it can be checked before it's copied into a spreadsheet or
// saved as CSV. The cells are the document's text, so they're only ever shown as plain text.
class TableDialog : public QDialog {
    Q_OBJECT

public:
    // `suggestedName` is the file name offered when saving, such as "A Paper, page 3.csv".
    TableDialog(const core::TableGrid &table, const QString &suggestedName, QWidget *parent = nullptr);

    // The rows as they'd be copied, with a cell's lines joined if that's ticked.
    QList<QStringList> rows() const;
    // Tab-separated text for pasting into cells, CSV and an HTML table, all on the clipboard at once.
    void copyToClipboard();
    // Writes the rows as CSV. On failure, `error` says why.
    bool saveTo(const QString &path, QString *error = nullptr);

    // A file name for a table from a page of a publication: its title, cut short, and the page, counting from 1.
    static QString suggestedFileName(const QString &title, int page);

private:
    void fill();
    void askAndSave();

    core::TableGrid m_table;
    QString m_suggestedName;
    QTableWidget *m_preview = nullptr;
    QCheckBox *m_joinLines = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace papertrail::ui
