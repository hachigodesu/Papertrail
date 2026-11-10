#include "ui/HighlightsWindow.h"

#include "core/db/Database.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"
#include "ui/LibrarySession.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <exception>
#include <memory>

namespace papertrail::ui {

using namespace core;

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

QString colourName(const QString &colour)
{
    const QStringList &colours = annotationColours();
    const QStringList names = {QObject::tr("Yellow"), QObject::tr("Green"), QObject::tr("Blue"), QObject::tr("Pink")};
    const qsizetype index = colours.indexOf(colour.toLower());
    return index >= 0 && index < names.size() ? names.at(index) : colour;
}

} // namespace

QString areaPicturePath(const QString &cacheDirectory, const Annotation &annotation)
{
    if (cacheDirectory.isEmpty() || annotation.uuid.isEmpty())
        return QString();
    // By uuid and when it last changed, so an area moved or edited isn't shown as it was.
    const QByteArray key = QCryptographicHash::hash((annotation.uuid + u'@' + QString::number(annotation.updatedAt)).toUtf8(),
                                                    QCryptographicHash::Sha1).toHex();
    return QDir(cacheDirectory).filePath(QStringLiteral("areas/%1.png").arg(QString::fromLatin1(key)));
}

HighlightsWindow::HighlightsWindow(LibrarySession &session, QWidget *parent)
    : QWidget(parent, Qt::Window), m_session(session)
{
    setWindowTitle(tr("Highlights and notes"));
    resize(1000, 640);
    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search highlights and notes"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_typing = new QTimer(this);
    m_typing->setSingleShot(true);
    m_typing->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, m_typing, qOverload<>(&QTimer::start));
    connect(m_typing, &QTimer::timeout, this, &HighlightsWindow::reload);

    auto *filters = new QHBoxLayout;
    const auto choice = [&](const QString &label) {
        auto *box = new QComboBox(this);
        box->setAccessibleName(label);
        box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        filters->addWidget(box);
        connect(box, &QComboBox::activated, this, &HighlightsWindow::reload);
        return box;
    };
    m_colour = choice(tr("Colour"));
    m_kind = choice(tr("Kind"));
    m_kind->addItem(tr("Every kind"), int(AnnotationKind::Any));
    m_kind->addItem(tr("Highlights"), int(AnnotationKind::Highlight));
    m_kind->addItem(tr("Underlines"), int(AnnotationKind::Underline));
    m_kind->addItem(tr("Notes"), int(AnnotationKind::Note));
    m_kind->addItem(tr("Areas"), int(AnnotationKind::Area));
    m_collection = choice(tr("Collection"));
    m_tag = choice(tr("Tag"));
    m_withNote = new QCheckBox(tr("With a note"), this);
    connect(m_withNote, &QCheckBox::toggled, this, &HighlightsWindow::reload);
    filters->addWidget(m_withNote);
    filters->addStretch();
    m_sort = choice(tr("Order"));
    m_sort->addItem(tr("Newest first"), false);
    m_sort->addItem(tr("By publication, in order"), true);
    layout->addLayout(filters);

    auto *only = new QHBoxLayout;
    m_only = new QLabel(this);
    m_only->setTextFormat(Qt::PlainText);
    only->addWidget(m_only, 1);
    m_showAll = new QPushButton(tr("Show all"), this);
    connect(m_showAll, &QPushButton::clicked, this, [this] { showOnlyPublication(0, QString()); });
    only->addWidget(m_showAll);
    layout->addLayout(only);
    m_only->hide();
    m_showAll->hide();

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_list = new QTreeWidget(splitter);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHeaderLabels({tr("Highlighted"), tr("Note"), tr("In"), tr("Page"), tr("Changed")});
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_list->header()->resizeSection(1, 220);
    m_list->header()->resizeSection(2, 240);
    connect(m_list, &QTreeWidget::itemActivated, this, &HighlightsWindow::openChosen);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] {
        m_open->setEnabled(m_list->currentItem() != nullptr);
        showPreview();
    });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &HighlightsWindow::showMenu);
    m_preview = new QLabel(splitter);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(0);
    m_preview->setTextFormat(Qt::PlainText);
    m_preview->setWordWrap(true);
    m_preview->hide();
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto *bottom = new QHBoxLayout;
    m_count = new QLabel(this);
    m_count->setTextFormat(Qt::PlainText);
    bottom->addWidget(m_count, 1);
    auto *exporting = new QPushButton(tr("Export shown..."), this);
    exporting->setToolTip(tr("Saves what's listed as Markdown, grouped by publication."));
    connect(exporting, &QPushButton::clicked, this, &HighlightsWindow::askToExport);
    bottom->addWidget(exporting);
    m_open = new QPushButton(tr("Open"), this);
    m_open->setToolTip(tr("Opens the document at this highlight or note."));
    connect(m_open, &QPushButton::clicked, this, &HighlightsWindow::openChosen);
    bottom->addWidget(m_open);
    auto *close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    fillChoices();
    reload();
}

HighlightsWindow::~HighlightsWindow()
{
    if (m_thread) {
        m_thread->disconnect(this);
        m_thread->wait();
        delete m_thread;
    }
}

void HighlightsWindow::fillChoices()
{
    const QVariant colour = m_colour->currentData();
    const QVariant collection = m_collection->currentData();
    const QVariant tag = m_tag->currentData();
    m_colour->clear();
    m_colour->addItem(tr("Every colour"), QString());
    QStringList colours = annotationColours();
    try {
        auto used = m_session.database().prepare("SELECT DISTINCT lower(colour) FROM annotation WHERE colour IS NOT NULL AND colour <> ''");
        while (used->step()) {
            if (!colours.contains(used->textAt(0)))
                colours << used->textAt(0);
        }
    } catch (const std::exception &) {
    }
    for (const QString &c : std::as_const(colours))
        m_colour->addItem(swatch(c), colourName(c), c);

    m_collection->clear();
    m_collection->addItem(tr("Every collection"), qint64(0));
    m_tag->clear();
    m_tag->addItem(tr("Every tag"), qint64(0));
    try {
        for (const Collection &c : CollectionRepository(m_session.database()).all())
            m_collection->addItem(c.path.isEmpty() ? c.name : c.path, c.id);
        TagRepository tags(m_session.database());
        for (const Tag &t : tags.all())
            m_tag->addItem(tags.path(t.id), t.id);
    } catch (const std::exception &) {
    }
    const auto restore = [](QComboBox *box, const QVariant &value) {
        const int index = box->findData(value);
        box->setCurrentIndex(index >= 0 ? index : 0);
    };
    restore(m_colour, colour);
    restore(m_collection, collection);
    restore(m_tag, tag);
}

AnnotationFilter HighlightsWindow::currentFilter() const
{
    AnnotationFilter filter;
    filter.search = m_search->text();
    filter.colour = m_colour->currentData().toString();
    filter.kind = AnnotationKind(m_kind->currentData().toInt());
    if (const qint64 collection = m_collection->currentData().toLongLong())
        filter.collectionIds = {collection};
    if (const qint64 tag = m_tag->currentData().toLongLong())
        filter.tagIds = {tag};
    filter.withNote = m_withNote->isChecked();
    filter.publicationId = m_onlyPublication;
    filter.byPublication = m_sort->currentData().toBool();
    filter.limit = kShownAtMost;
    return filter;
}

void HighlightsWindow::showOnlyPublication(qint64 publicationId, const QString &title)
{
    m_onlyPublication = publicationId;
    m_only->setText(tr("Only in %1").arg(oneLine(title, 120)));
    m_only->setVisible(publicationId != 0);
    m_showAll->setVisible(publicationId != 0);
    if (publicationId != 0)
        m_sort->setCurrentIndex(1); // In the order they come in it.
    reload();
}

void HighlightsWindow::reload()
{
    m_typing->stop();
    if (m_thread) {
        m_again = true;
        return;
    }
    try {
        m_stamp = m_session.database().changeStamp();
    } catch (const std::exception &) {
    }
    const AnnotationFilter filter = currentFilter();
    const QString path = m_session.paths().database;
    auto found = std::make_shared<QList<AnnotationListing>>();
    auto total = std::make_shared<qint64>(0);
    auto failure = std::make_shared<QString>();
    m_thread = QThread::create([path, filter, found, total, failure] {
        try {
            Database db(path);
            *found = AnnotationRepository(db).everywhere(filter, total.get());
        } catch (const std::exception &error) {
            *failure = QString::fromUtf8(error.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, found, total, failure] {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (m_again) {
            m_again = false;
            reload();
            return;
        }
        if (!failure->isEmpty()) {
            m_list->clear();
            m_found.clear();
            m_count->setText(tr("The highlights couldn't be read: %1").arg(*failure));
            m_open->setEnabled(false);
        } else {
            showFound(*found, *total);
        }
        emit loaded();
    });
    m_thread->start();
}

void HighlightsWindow::showFound(const QList<AnnotationListing> &found, qint64 total)
{
    const qint64 chosen = chosenId();
    m_found = found;
    m_list->clear();
    const QLocale locale;
    for (const AnnotationListing &listing : found) {
        const Annotation &annotation = listing.annotation;
        const int page = annotation.firstPage();
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
        if (annotation.id == chosen)
            m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    m_open->setEnabled(m_list->currentItem() != nullptr);
    showPreview();

    const AnnotationFilter filter = currentFilter();
    const bool narrowed = !filter.search.trimmed().isEmpty() || !filter.colour.isEmpty() || filter.kind != AnnotationKind::Any
                          || !filter.collectionIds.isEmpty() || !filter.tagIds.isEmpty() || filter.withNote || filter.publicationId;
    if (total == 0)
        m_count->setText(narrowed ? tr("Nothing matches.") : tr("There are no highlights or notes yet. Make them in any open document."));
    else if (total > found.size())
        m_count->setText(tr("Showing %1 of %2. Search or choose to find others.").arg(locale.toString(found.size()), locale.toString(total)));
    else
        m_count->setText(total == 1 ? tr("1 highlight or note") : tr("%1 highlights and notes").arg(locale.toString(total)));
}

void HighlightsWindow::showPreview()
{
    const std::optional<AnnotationListing> listing = listingOf(chosenId());
    if (!listing || !listing->annotation.isArea()) {
        m_preview->hide();
        return;
    }
    const QString path = areaPicturePath(m_session.paths().cacheDirectory, listing->annotation);
    const QPixmap picture(path);
    if (picture.isNull()) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(tr("A picture of this area is shown once its document has been opened."));
    } else {
        const QSize most = m_preview->size().isEmpty() ? QSize(600, 240) : m_preview->size();
        m_preview->setPixmap(picture.scaled(most.boundedTo(picture.size()), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    m_preview->show();
}

qint64 HighlightsWindow::chosenId() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return item ? item->data(0, kIdRole).toLongLong() : 0;
}

std::optional<AnnotationListing> HighlightsWindow::listingOf(qint64 annotationId) const
{
    for (const AnnotationListing &listing : m_found) {
        if (listing.annotation.id == annotationId)
            return listing;
    }
    return std::nullopt;
}

QList<qint64> HighlightsWindow::shownIds() const
{
    QList<qint64> ids;
    for (const AnnotationListing &listing : m_found)
        ids << listing.annotation.id;
    return ids;
}

void HighlightsWindow::openChosen()
{
    if (const qint64 id = chosenId())
        emit openRequested(id);
}

void HighlightsWindow::showMenu(const QPoint &position)
{
    QTreeWidgetItem *item = m_list->itemAt(position);
    if (!item)
        return;
    m_list->setCurrentItem(item);
    const qint64 id = chosenId();
    const std::optional<AnnotationListing> listing = listingOf(id);
    if (!listing)
        return;
    QMenu menu(this);
    menu.addAction(tr("Open"), this, &HighlightsWindow::openChosen);
    menu.addAction(tr("Copy text"), this, [listing] {
        QString text = listing->annotation.selectedText();
        if (!listing->annotation.note.isEmpty())
            text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + listing->annotation.note;
        QApplication::clipboard()->setText(text);
    });
    QMenu *colours = menu.addMenu(tr("Colour"));
    for (const QString &colour : annotationColours())
        colours->addAction(swatch(colour), colourName(colour), this, [this, id, colour] { setColourOf(id, colour); });
    menu.addAction(tr("Edit note..."), this, [this, id, listing] {
        bool ok = false;
        const QString note = QInputDialog::getMultiLineText(this, tr("Note"), tr("Note:"), listing->annotation.note, &ok);
        if (ok)
            setNoteOf(id, note);
    });
    menu.addSeparator();
    menu.addAction(tr("Delete..."), this, [this, id] {
        QMessageBox box(QMessageBox::Question, tr("Delete"), tr("Delete this highlight and its note? This can't be undone."),
                        QMessageBox::NoButton, this);
        box.setTextFormat(Qt::PlainText);
        QPushButton *yes = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() == yes)
            deleteWithoutAsking(id);
    });
    menu.exec(m_list->viewport()->mapToGlobal(position));
}

void HighlightsWindow::setNoteOf(qint64 annotationId, const QString &note)
{
    try {
        AnnotationRepository(m_session.database()).setNote(annotationId, note);
    } catch (const std::exception &error) {
        m_count->setText(tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    changed(annotationId);
}

void HighlightsWindow::setColourOf(qint64 annotationId, const QString &colour)
{
    try {
        AnnotationRepository(m_session.database()).setColour(annotationId, colour);
    } catch (const std::exception &error) {
        m_count->setText(tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    changed(annotationId);
}

void HighlightsWindow::deleteWithoutAsking(qint64 annotationId)
{
    const std::optional<AnnotationListing> listing = listingOf(annotationId);
    try {
        AnnotationRepository(m_session.database()).remove(annotationId);
    } catch (const std::exception &error) {
        m_count->setText(tr("That couldn't be deleted: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    if (listing)
        emit annotationsChanged(listing->annotation.contentHash);
    reload();
}

void HighlightsWindow::changed(qint64 annotationId)
{
    if (const std::optional<AnnotationListing> listing = listingOf(annotationId))
        emit annotationsChanged(listing->annotation.contentHash);
    reload();
}

bool HighlightsWindow::exportShown(const QString &path, QString *error)
{
    // Grouped by publication, in the order each first comes in the list.
    QList<qint64> order;
    QHash<qint64, QList<Annotation>> byPublication;
    for (const AnnotationListing &listing : std::as_const(m_found)) {
        const qint64 publication = listing.annotation.publicationId;
        if (!byPublication.contains(publication))
            order << publication;
        byPublication[publication] << listing.annotation;
    }
    QString text;
    try {
        PublicationRepository publications(m_session.database());
        for (const qint64 id : std::as_const(order)) {
            const std::optional<Publication> publication = publications.get(id);
            if (!publication)
                continue;
            if (!text.isEmpty())
                text += QStringLiteral("\n\n");
            text += annotationsToMarkdown(*publication, byPublication.value(id));
        }
    } catch (const std::exception &failure) {
        if (error)
            *error = QString::fromUtf8(failure.what());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void HighlightsWindow::askToExport()
{
    if (m_found.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export shown"), tr("Highlights.md"), tr("Markdown (*.md)"));
    if (path.isEmpty())
        return;
    QString error;
    if (exportShown(path, &error))
        m_count->setText(tr("Saved %1.").arg(QFileInfo(path).fileName()));
    else
        m_count->setText(tr("That couldn't be saved: %1").arg(error));
}

void HighlightsWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Highlights made in a document meanwhile are shown on coming back, and new collections and tags offered.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        try {
            if (m_session.database().changeStamp() != m_stamp) {
                fillChoices();
                reload();
            }
        } catch (const std::exception &) {
        }
    }
}

} // namespace papertrail::ui
