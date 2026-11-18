#include "ui/TableDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

namespace papertrail::ui {

TableDialog::TableDialog(const core::TableGrid &table, const QString &suggestedName, QWidget *parent)
    : QDialog(parent), m_table(table), m_suggestedName(suggestedName)
{
    setWindowTitle(tr("Copy as a table"));
    auto *layout = new QVBoxLayout(this);

    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_preview = new QTableWidget(this);
    m_preview->setObjectName(QStringLiteral("tablePreview"));
    m_preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_preview->setWordWrap(false);
    m_preview->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    layout->addWidget(m_preview, 1);

    m_joinLines = new QCheckBox(tr("Join lines of a cell"), this);
    m_joinLines->setObjectName(QStringLiteral("joinLines"));
    m_joinLines->setToolTip(tr("Takes a cell's text over two lines or more as one cell, rather than a row for each line."));
    m_joinLines->setChecked(true);
    m_joinLines->setEnabled(m_table.continues.contains(true));
    connect(m_joinLines, &QCheckBox::toggled, this, &TableDialog::fill);
    layout->addWidget(m_joinLines);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    copy->setObjectName(QStringLiteral("copyTable"));
    copy->setToolTip(tr("Puts the table on the clipboard, to paste into a spreadsheet's cells."));
    QPushButton *save = buttons->addButton(tr("Save as CSV..."), QDialogButtonBox::ActionRole);
    save->setObjectName(QStringLiteral("saveTable"));
    buttons->addButton(QDialogButtonBox::Close);
    connect(copy, &QPushButton::clicked, this, [this] {
        copyToClipboard();
        m_status->setText(tr("Copied. Paste it into a spreadsheet."));
    });
    connect(save, &QPushButton::clicked, this, &TableDialog::askAndSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    fill();
    resize(720, 420);
}

QList<QStringList> TableDialog::rows() const
{
    return core::tableRows(m_table, m_joinLines->isChecked());
}

void TableDialog::fill()
{
    const QList<QStringList> shown = rows();
    m_preview->clear();
    m_preview->setRowCount(int(shown.size()));
    m_preview->setColumnCount(m_table.columns);
    for (int r = 0; r < shown.size(); ++r) {
        for (int c = 0; c < m_table.columns && c < shown.at(r).size(); ++c) {
            // A table widget's items are never read as rich text.
            auto *item = new QTableWidgetItem(shown.at(r).at(c));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_preview->setItem(r, c, item);
        }
    }
    m_preview->resizeColumnsToContents();
    const QString rowsText = tr("%n row(s)", nullptr, int(shown.size()));
    const QString columnsText = tr("%n column(s)", nullptr, m_table.columns);
    m_summary->setText(m_table.ruled ? tr("%1 and %2, from the rules drawn between them.").arg(rowsText, columnsText)
                                     : tr("%1 and %2, from how the words line up.").arg(rowsText, columnsText));
    m_status->clear();
}

void TableDialog::copyToClipboard()
{
    const QList<QStringList> copied = rows();
    auto *data = new QMimeData;
    data->setText(core::tableAsTsv(copied));
    data->setData(QStringLiteral("text/csv"), core::tableAsCsv(copied));
    data->setHtml(core::tableAsHtml(copied));
    QApplication::clipboard()->setMimeData(data);
}

bool TableDialog::saveTo(const QString &path, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(core::tableAsCsv(rows())) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void TableDialog::askAndSave()
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QFileDialog::getSaveFileName(this, tr("Save as CSV"), QDir(folder).filePath(m_suggestedName),
                                                tr("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".csv");
    QString error;
    if (!saveTo(path, &error)) {
        QMessageBox box(QMessageBox::Warning, tr("Save as CSV"), tr("The table couldn't be saved: %1").arg(error),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    m_status->setText(tr("Saved as %1.").arg(QFileInfo(path).fileName()));
}

QString TableDialog::suggestedFileName(const QString &title, int page)
{
    QString cleaned;
    for (const QChar c : title.simplified()) {
        const bool kept = c.isLetterOrNumber() || c == u'-' || c == u'_' || c == u'(' || c == u')' || c == u',';
        cleaned += kept ? c : QChar(u' ');
    }
    cleaned = cleaned.simplified().left(80).trimmed();
    if (cleaned.isEmpty())
        cleaned = tr("Table");
    return tr("%1, page %2.csv").arg(cleaned).arg(page + 1);
}

} // namespace papertrail::ui
