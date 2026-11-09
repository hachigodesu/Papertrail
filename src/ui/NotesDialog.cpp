#include "ui/NotesDialog.h"

#include "core/repo/AnnotationRepository.h"
#include "ui/LibrarySession.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace papertrail::ui {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

QIcon swatch(const QString &colour)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(colour.isEmpty() ? QStringLiteral("#ffd54f") : colour));
    painter.drawRoundedRect(QRectF(0, 0, 12, 12), 2, 2);
    return QIcon(pixmap);
}

// Text from a document can run long and over several lines, which a list shows on one.
QString oneLine(const QString &text, int most = 300)
{
    QString line = text.simplified();
    if (line.size() > most)
        line = line.left(most - 1) + QChar(0x2026);
    return line;
}

} // namespace

NotesDialog::NotesDialog(LibrarySession &session, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
{
    setWindowTitle(tr("Highlights and notes"));
    resize(900, 560);
    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search highlights and notes"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_typing = new QTimer(this);
    m_typing->setSingleShot(true);
    m_typing->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, m_typing, qOverload<>(&QTimer::start));
    connect(m_typing, &QTimer::timeout, this, &NotesDialog::reload);

    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setHeaderLabels({tr("Highlighted"), tr("Note"), tr("In"), tr("Page"), tr("Changed")});
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_list->header()->resizeSection(1, 200);
    m_list->header()->resizeSection(2, 220);
    connect(m_list, &QTreeWidget::itemActivated, this, &NotesDialog::openChosen);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] { m_open->setEnabled(m_list->currentItem() != nullptr); });
    layout->addWidget(m_list, 1);

    auto *bottom = new QHBoxLayout;
    m_count = new QLabel(this);
    m_count->setTextFormat(Qt::PlainText);
    bottom->addWidget(m_count, 1);
    m_open = new QPushButton(tr("Open"), this);
    m_open->setToolTip(tr("Opens the document at this highlight or note."));
    connect(m_open, &QPushButton::clicked, this, &NotesDialog::openChosen);
    bottom->addWidget(m_open);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    reload();
}

void NotesDialog::reload()
{
    m_typing->stop();
    m_list->clear();
    qint64 total = 0;
    QList<core::AnnotationListing> found;
    try {
        found = core::AnnotationRepository(m_session.database()).everywhere(m_search->text(), kShownAtMost, &total);
    } catch (const std::exception &error) {
        m_count->setText(tr("The highlights couldn't be read: %1").arg(QString::fromUtf8(error.what())));
        m_open->setEnabled(false);
        return;
    }
    const QLocale locale;
    for (const core::AnnotationListing &listing : found) {
        const core::Annotation &annotation = listing.annotation;
        const int page = annotation.firstPage();
        // An area says so, with any words inside it after.
        const QString words = annotation.selectedText().simplified();
        const QString highlighted = !annotation.isArea() ? oneLine(annotation.selectedText())
                                    : words.isEmpty()   ? tr("Area")
                                                        : tr("Area: %1").arg(oneLine(words));
        // All shown as plain text, since it comes from documents.
        auto *item = new QTreeWidgetItem(m_list, {highlighted, oneLine(annotation.note), oneLine(listing.publicationTitle, 120),
                                                  page >= 0 ? QString::number(page + 1) : QString(),
                                                  locale.toString(QDateTime::fromMSecsSinceEpoch(annotation.updatedAt).date(), QLocale::ShortFormat)});
        item->setIcon(0, swatch(annotation.colour));
        item->setToolTip(0, annotation.selectedText().left(2000));
        item->setToolTip(1, annotation.note.left(2000));
        item->setToolTip(2, listing.publicationTitle);
        item->setData(0, kIdRole, annotation.id);
    }
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    m_open->setEnabled(m_list->currentItem() != nullptr);

    const bool searching = !m_search->text().trimmed().isEmpty();
    if (total == 0)
        m_count->setText(searching ? tr("Nothing matches.") : tr("There are no highlights or notes yet. Make them in any open document."));
    else if (total > found.size())
        m_count->setText(tr("Showing the newest %1 of %2. Search to find others.").arg(locale.toString(found.size()), locale.toString(total)));
    else
        m_count->setText(total == 1 ? tr("1 highlight or note") : tr("%1 highlights and notes").arg(locale.toString(total)));
}

void NotesDialog::openChosen()
{
    if (QTreeWidgetItem *item = m_list->currentItem())
        emit openRequested(item->data(0, kIdRole).toLongLong());
}

} // namespace papertrail::ui
