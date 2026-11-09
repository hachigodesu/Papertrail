#include "ui/DetailsPanel.h"

#include "core/repo/AnnotationRepository.h"
#include "core/repo/FullTextIndex.h"
#include "core/repo/NoticeRepository.h"
#include "core/repo/SummaryRepository.h"
#include "core/repo/VolumeArticles.h"
#include "summary/SummaryBatches.h"
#include "summary/SummaryRequest.h"

#include "core/repo/PublicationMerge.h"
#include "metadata/Matching.h"
#include "ui/CoverCache.h"
#include "ui/Delegates.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

namespace papertrail::ui {

using namespace core;

namespace {

// Text from documents goes through this, so that Qt can't take it for rich text. Metadata that
// looks like HTML could otherwise change the layout or try to load images.
QLabel *plainLabel(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QLabel *sectionHeading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

void clearLayout(QLayout *layout)
{
    while (layout->count() > 0) {
        QLayoutItem *item = layout->takeAt(0);
        if (QWidget *widget = item->widget())
            delete widget;
        if (QLayout *inner = item->layout())
            clearLayout(inner);
        delete item;
    }
}

QString reviewReason(PublicationRepository &publications, qint64 id)
{
    const QList<ProvenanceEntry> entries = publications.provenance(id, QStringLiteral("title"));
    if (!entries.isEmpty()) {
        const QString source = entries.last().source;
        if (source == QLatin1StringView("first_page"))
            return DetailsPanel::tr("The title was worked out from the first page, and may be wrong.");
        if (source == QLatin1StringView("file_name"))
            return DetailsPanel::tr("The title comes from the file name, because the file doesn't state one.");
    }
    return DetailsPanel::tr("The publication type is a best guess.");
}

} // namespace

DetailsPanel::DetailsPanel(LibrarySession &session, CoverCache &covers, QWidget *parent)
    : QWidget(parent), m_session(session), m_covers(covers)
{
    // An article's own cover may only be made after it's first shown here.
    connect(&m_covers, &CoverCache::coverReady, this, [this](const QString &key) {
        if (!m_coverSha.isEmpty() && key == m_coverSha)
            updateCover();
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);

    auto *nothing = new QLabel(tr("Select a publication to see its details."), this);
    nothing->setAlignment(Qt::AlignCenter);
    nothing->setWordWrap(true);
    nothing->setMargin(16);
    m_pages->addWidget(nothing);

    buildSinglePage();
    buildManyPage();

    m_notesTimer = new QTimer(this);
    m_notesTimer->setSingleShot(true);
    m_notesTimer->setInterval(1000);
    connect(m_notesTimer, &QTimer::timeout, this, &DetailsPanel::saveNotes);
}

void DetailsPanel::buildSinglePage()
{
    auto *contents = new QWidget;
    auto *column = new QVBoxLayout(contents);
    column->setContentsMargins(10, 10, 10, 10);
    column->setSpacing(8);

    m_cover = new QLabel;
    m_cover->setAlignment(Qt::AlignCenter);
    m_cover->setAccessibleName(tr("Front page"));
    column->addWidget(m_cover);

    m_title = plainLabel();
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    m_title->setFont(titleFont);
    column->addWidget(m_title);

    m_authors = plainLabel();
    column->addWidget(m_authors);

    m_review = new QFrame;
    static_cast<QFrame *>(m_review)->setFrameShape(QFrame::StyledPanel);
    auto *reviewLayout = new QVBoxLayout(m_review);
    reviewLayout->addWidget(sectionHeading(tr("Needs checking")));
    m_reviewReason = plainLabel();
    reviewLayout->addWidget(m_reviewReason);
    auto *checked = new QPushButton(tr("Mark as checked"));
    connect(checked, &QPushButton::clicked, this, [this] {
        if (m_id) {
            m_session.publications().setNeedsReview({*m_id}, false);
            reload();
            emit libraryEdited();
        }
    });
    reviewLayout->addWidget(checked);
    column->addWidget(m_review);

    // One button if there's one format, and one for each format if there's a choice.
    m_openButtons = new QHBoxLayout;
    column->addLayout(m_openButtons);

    auto *actions = new QHBoxLayout;
    m_open = new QPushButton(tr("Open"));
    m_showInFolder = new QPushButton(tr("Show in folder"));
    m_edit = new QPushButton(tr("Edit..."));
    m_cite = new QPushButton(tr("Cite"));
    auto *citeMenu = new QMenu(m_cite);
    m_cite->setMenu(citeMenu);
    connect(citeMenu, &QMenu::aboutToShow, this, [this, citeMenu] {
        citeMenu->clear();
        if (m_id)
            emit citeMenuRequested(citeMenu, *m_id);
    });
    actions->addWidget(m_open);
    actions->addWidget(m_showInFolder);
    actions->addWidget(m_edit);
    actions->addWidget(m_cite);
    connect(m_open, &QPushButton::clicked, this, [this] { if (m_id) emit openRequested(*m_id); });
    connect(m_showInFolder, &QPushButton::clicked, this, [this] { if (m_id) emit showInFolderRequested(*m_id); });
    connect(m_edit, &QPushButton::clicked, this, [this] { if (m_id) emit editRequested(*m_id); });
    column->addLayout(actions);

    auto *personal = new QFormLayout;
    m_readStatus = new QComboBox;
    for (const ReadStatus status : {ReadStatus::Unread, ReadStatus::Reading, ReadStatus::Read})
        m_readStatus->addItem(readStatusLabel(status), int(status));
    connect(m_readStatus, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading)
            return;
        const auto status = ReadStatus(m_readStatus->currentData().toInt());
        changePublication([status](Publication &p) { p.readStatus = status; });
    });
    personal->addRow(tr("Status"), m_readStatus);

    m_rating = new QComboBox;
    m_rating->addItem(tr("No rating"), 0);
    for (int stars = 1; stars <= 5; ++stars)
        m_rating->addItem(QString::number(stars), stars);
    connect(m_rating, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading)
            return;
        const int stars = m_rating->currentData().toInt();
        changePublication([stars](Publication &p) { p.rating = stars == 0 ? std::nullopt : std::optional<int>(stars); });
    });
    personal->addRow(tr("Rating"), m_rating);
    column->addLayout(personal);

    column->addWidget(sectionHeading(tr("Tags")));
    m_tagRows = new QVBoxLayout;
    m_tagRows->setSpacing(2);
    column->addLayout(m_tagRows);
    m_collections = plainLabel();
    m_collections->setWordWrap(true);
    column->addWidget(m_collections);

    // A summary from Claude, kept apart from everything else here because it's generated.
    column->addWidget(sectionHeading(tr("Summary")));
    // One asked for at half price, on its way or back and waiting to be reviewed.
    m_summaryPending = plainLabel();
    m_summaryPending->setWordWrap(true);
    column->addWidget(m_summaryPending);
    m_summaries = new QPushButton;
    connect(m_summaries, &QPushButton::clicked, this, [this] {
        if (m_id)
            emit summariesRequested(*m_id);
    });
    auto *pendingRow = new QHBoxLayout;
    pendingRow->addWidget(m_summaries);
    pendingRow->addStretch();
    column->addLayout(pendingRow);
    m_summaryInfo = plainLabel();
    m_summaryInfo->setWordWrap(true);
    column->addWidget(m_summaryInfo);
    m_summaryView = new QPlainTextEdit;
    m_summaryView->setReadOnly(true);
    m_summaryView->setMinimumHeight(140);
    m_summaryView->setAccessibleName(tr("Summary generated by Claude"));
    column->addWidget(m_summaryView);
    auto *summaryButtons = new QHBoxLayout;
    m_summarize = new QPushButton(tr("Summarize with Claude..."));
    m_summarize->setToolTip(tr("Uses your own Claude API key. It shows what will be sent and what it may cost first."));
    connect(m_summarize, &QPushButton::clicked, this, [this] {
        if (m_id)
            emit summarizeRequested(*m_id);
    });
    m_deleteSummary = new QPushButton(tr("Delete summary"));
    connect(m_deleteSummary, &QPushButton::clicked, this, [this] {
        if (!m_id)
            return;
        QMessageBox box(QMessageBox::Question, tr("Delete summary"), tr("Delete this summary? Getting another would cost money again."),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Yes)
            return;
        try {
            SummaryRepository(m_session.database()).remove(*m_id);
            reload();
        } catch (const std::exception &error) {
            QMessageBox::warning(this, tr("Delete summary"), tr("That couldn't be deleted: %1").arg(QString::fromUtf8(error.what())));
        }
    });
    summaryButtons->addWidget(m_summarize);
    summaryButtons->addWidget(m_deleteSummary);
    summaryButtons->addStretch();
    column->addLayout(summaryButtons);

    // Where a search of the library found something inside this document.
    m_matchesSection = new QWidget;
    auto *found = new QVBoxLayout(m_matchesSection);
    found->setContentsMargins(0, 0, 0, 0);
    found->addWidget(sectionHeading(tr("Found inside")));
    m_matchList = new QListWidget; // List items show text as it is, so a document's text can't become formatting.
    m_matchList->setAccessibleName(tr("Where the search found this inside the document"));
    m_matchList->setMaximumHeight(190);
    m_matchList->setWordWrap(true);
    m_matchList->setToolTip(tr("Double-click to open it at this page."));
    connect(m_matchList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (m_id)
            emit openMatchRequested(*m_id, item->data(Qt::UserRole).toLongLong(), item->data(Qt::UserRole + 1).toInt(), m_contentsText);
    });
    found->addWidget(m_matchList);
    column->addWidget(m_matchesSection);
    m_matchesSection->hide();

    // An issue, proceedings or book lists its articles, and an article leads back to it.
    m_articlesSection = new QWidget;
    auto *articles = new QVBoxLayout(m_articlesSection);
    articles->setContentsMargins(0, 0, 0, 0);
    articles->addWidget(sectionHeading(tr("Articles")));
    m_articleList = new QListWidget; // List items show text as it is, so a title can't become formatting.
    m_articleList->setAccessibleName(tr("Articles in this publication"));
    m_articleList->setMaximumHeight(320);
    m_articleList->setWordWrap(true);
    m_articleList->setItemDelegate(new ArticleDelegate(m_articleList));
    m_articleList->setResizeMode(QListView::Adjust); // Titles wrap to the panel's width.
    m_articleList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_articleList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_articleList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_articleList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        emit openInVolumeRequested(item->data(Qt::UserRole).toLongLong());
    });
    connect(m_articleList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = m_articleList->itemAt(position);
        if (!item)
            return;
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        QMenu menu(this);
        menu.addAction(tr("Open"), this, [this, id] { emit openInVolumeRequested(id); });
        menu.addAction(tr("Open on its own"), this, [this, id] { emit openRequested(id); });
        menu.addAction(tr("Show its details"), this, [this, id] { showPublication(id); });
        menu.addSeparator();
        menu.addAction(tr("Delete article..."), this, [this, id] { emit deleteArticleRequested(id); });
        menu.exec(m_articleList->viewport()->mapToGlobal(position));
    });
    articles->addWidget(m_articleList);
    auto *articleButtons = new QHBoxLayout;
    auto *fromContents = new QPushButton(tr("From contents..."));
    fromContents->setToolTip(tr("Proposes articles from this file's table of contents, for you to check."));
    connect(fromContents, &QPushButton::clicked, this, [this] {
        if (m_id)
            emit articlesFromContentsRequested(*m_id);
    });
    auto *addArticle = new QPushButton(tr("Add..."));
    addArticle->setToolTip(tr("Adds one article by hand."));
    connect(addArticle, &QPushButton::clicked, this, [this] {
        if (m_id)
            emit addArticleRequested(*m_id);
    });
    articleButtons->addWidget(fromContents);
    articleButtons->addWidget(addArticle);
    articleButtons->addStretch();
    articles->addLayout(articleButtons);
    column->addWidget(m_articlesSection);

    m_partOf = plainLabel();
    m_partOf->setWordWrap(true);
    column->addWidget(m_partOf);
    auto *partOfButtons = new QHBoxLayout;
    m_showVolume = new QPushButton(tr("Show what it's part of"));
    connect(m_showVolume, &QPushButton::clicked, this, [this] {
        if (!m_id)
            return;
        const std::optional<Publication> article = m_session.publications().get(*m_id);
        if (article && article->parentId)
            showPublication(*article->parentId);
    });
    m_deleteArticle = new QPushButton(tr("Delete article..."));
    connect(m_deleteArticle, &QPushButton::clicked, this, [this] {
        if (m_id)
            emit deleteArticleRequested(*m_id);
    });
    partOfButtons->addWidget(m_showVolume);
    partOfButtons->addWidget(m_deleteArticle);
    partOfButtons->addStretch();
    column->addLayout(partOfButtons);
    m_tagEdit = new QLineEdit;
    m_tagEdit->setPlaceholderText(tr("Add a tag, such as Engineering > FEM"));
    m_tagEdit->setAccessibleName(tr("Add a tag"));
    connect(m_tagEdit, &QLineEdit::returnPressed, this, [this] {
        const QString path = m_tagEdit->text().trimmed();
        if (!path.isEmpty()) {
            addTag(path);
            m_tagEdit->clear();
        }
    });
    column->addWidget(m_tagEdit);

    column->addWidget(sectionHeading(tr("Details")));
    m_facts = new QFormLayout;
    m_facts->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_facts->setRowWrapPolicy(QFormLayout::WrapLongRows);
    column->addLayout(m_facts);

    m_preprint = new QCheckBox(tr("Preprint"));
    m_preprint->setToolTip(tr("Not yet formally published. Moves this to the Preprints tab."));
    connect(m_preprint, &QCheckBox::toggled, this, [this](bool marked) {
        if (!m_loading)
            setMarkedAsPreprint(marked);
    });
    column->addWidget(m_preprint);

    m_preprintHint = plainLabel();
    column->addWidget(m_preprintHint);

    // A retraction or the like, said plainly near the top, since it changes how the work can be relied on.
    m_noticeWarning = plainLabel();
    QFont warningFont = m_noticeWarning->font();
    warningFont.setBold(true);
    m_noticeWarning->setFont(warningFont);
    column->addWidget(m_noticeWarning);

    m_undoRow = new QWidget;
    auto *undoLayout = new QHBoxLayout(m_undoRow);
    undoLayout->setContentsMargins(0, 0, 0, 0);
    m_undoLabel = plainLabel();
    undoLayout->addWidget(m_undoLabel, 1);
    auto *undo = new QPushButton(tr("Undo"));
    undo->setToolTip(tr("Puts back the details from before this lookup."));
    connect(undo, &QPushButton::clicked, this, &DetailsPanel::undoLookup);
    undoLayout->addWidget(undo);
    column->addWidget(m_undoRow);

    column->addWidget(sectionHeading(tr("Notes")));
    m_notes = new QPlainTextEdit;
    m_notes->setTabChangesFocus(true);
    m_notes->setMinimumHeight(80);
    m_notes->setMaximumHeight(160);
    m_notes->setAccessibleName(tr("Notes"));
    connect(m_notes, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading) {
            m_notesDirty = true;
            m_notesTimer->start();
        }
    });
    column->addWidget(m_notes);

    column->addWidget(sectionHeading(tr("Files")));
    m_fileRows = new QVBoxLayout;
    m_fileRows->setSpacing(6);
    column->addLayout(m_fileRows);
    column->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setWidget(contents);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Wide enough for the buttons and lists, since sideways scrolling is off.
    scroll->setMinimumWidth(contents->minimumSizeHint().width() + scroll->verticalScrollBar()->sizeHint().width() + 2);
    m_pages->addWidget(scroll);
}

void DetailsPanel::buildManyPage()
{
    auto *page = new QWidget;
    auto *column = new QVBoxLayout(page);
    column->setContentsMargins(10, 10, 10, 10);
    column->setSpacing(8);

    m_manyCount = new QLabel;
    QFont font = m_manyCount->font();
    font.setBold(true);
    m_manyCount->setFont(font);
    column->addWidget(m_manyCount);

    column->addWidget(new QLabel(tr("Add a tag to all of them:")));
    m_manyTagEdit = new QLineEdit;
    m_manyTagEdit->setPlaceholderText(tr("Engineering > FEM"));
    m_manyTagEdit->setAccessibleName(tr("Add a tag to all selected"));
    connect(m_manyTagEdit, &QLineEdit::returnPressed, this, [this] {
        const QString path = m_manyTagEdit->text().trimmed();
        if (!path.isEmpty()) {
            emit bulkTagRequested(path);
            m_manyTagEdit->clear();
        }
    });
    column->addWidget(m_manyTagEdit);

    column->addWidget(new QLabel(tr("Set their status:")));
    auto *statuses = new QHBoxLayout;
    for (const ReadStatus status : {ReadStatus::Unread, ReadStatus::Reading, ReadStatus::Read}) {
        auto *button = new QPushButton(readStatusLabel(status));
        connect(button, &QPushButton::clicked, this, [this, status] { emit bulkReadStatusRequested(status); });
        statuses->addWidget(button);
    }
    column->addLayout(statuses);

    auto *checked = new QPushButton(tr("Mark all as checked"));
    connect(checked, &QPushButton::clicked, this, &DetailsPanel::bulkMarkCheckedRequested);
    column->addWidget(checked);
    column->addStretch();
    m_pages->addWidget(page);
}

void DetailsPanel::showNothing()
{
    saveNotes();
    m_id.reset();
    m_coverSha.clear();
    m_pages->setCurrentIndex(0);
}

void DetailsPanel::showSelectionCount(qint64 count)
{
    saveNotes();
    m_id.reset();
    m_coverSha.clear();
    m_manyCount->setText(tr("%1 publications selected").arg(QLocale().toString(count)));
    refreshTagCompleter(m_manyTagEdit);
    m_pages->setCurrentIndex(2);
}

void DetailsPanel::setContentsSearch(const QString &match, const QString &readerText)
{
    if (match == m_contentsMatch && readerText == m_contentsText)
        return;
    m_contentsMatch = match;
    m_contentsText = readerText;
    if (m_id)
        reload();
}

void DetailsPanel::showPublication(qint64 id)
{
    saveNotes();
    m_id = id;
    m_pages->setCurrentIndex(1);
    reload();
}

void DetailsPanel::updateCover()
{
    m_covers.cover(m_coverSha); // Has an article's own cover made, if it hasn't been yet.
    const QImage cover = m_covers.fullCover(m_coverSha);
    if (cover.isNull()) {
        m_cover->clear();
        return;
    }
    const qreal ratio = devicePixelRatioF();
    QPixmap scaled = QPixmap::fromImage(cover.scaled(QSize(220, 300) * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    scaled.setDevicePixelRatio(ratio);
    if (const QString label = coverNoticeLabel(m_coverNotice); !label.isEmpty()) {
        QPainter painter(&scaled);
        drawNoticeBar(&painter, QRect(QPoint(0, 0), scaled.deviceIndependentSize().toSize()), label);
    }
    m_cover->setPixmap(scaled);
}

void DetailsPanel::reload()
{
    if (!m_id)
        return;
    const std::optional<Publication> found = m_session.publications().get(*m_id);
    if (!found) {
        showNothing();
        return;
    }
    const Publication &p = *found;

    m_loading = true;
    m_title->setText(p.title);

    QStringList authors;
    for (const PersonName &person : (p.authors.isEmpty() ? p.editors : p.authors))
        authors << person.display();
    m_authors->setText(authors.join(QStringLiteral("; ")));
    m_authors->setVisible(!authors.isEmpty());

    m_review->setVisible(p.needsReview);
    if (p.needsReview)
        m_reviewReason->setText(reviewReason(m_session.publications(), p.id));

    m_preprint->setChecked(p.type == PublicationType::Preprint);

    // It's a suggestion. Only ticking the box makes something a preprint.
    const QString hintSource = QJsonDocument::fromJson(p.extraJson.toUtf8()).object().value(QStringLiteral("preprintHint")).toString();
    m_preprintHint->setText(tr("%1 lists this as a preprint.").arg(metadata::providerLabel(hintSource)));
    m_preprintHint->setVisible(!hintSource.isEmpty() && p.type != PublicationType::Preprint);

    QList<Notice> notices;
    try {
        notices = NoticeRepository(m_session.database()).forPublication(p.id);
    } catch (const std::exception &) {
    }
    QStringList warnings;
    // The cover shows the gravest of them, as the library does.
    m_coverNotice.clear();
    for (const QString &kind : {QStringLiteral("retraction"), QStringLiteral("partial_retraction"), QStringLiteral("withdrawal"),
                                QStringLiteral("removal")}) {
        const bool found = std::any_of(notices.cbegin(), notices.cend(), [&](const Notice &notice) { return notice.kind == kind; });
        if (found && m_coverNotice.isEmpty())
            m_coverNotice = kind == QLatin1StringView("removal") ? QStringLiteral("withdrawal") : kind;
    }
    for (const Notice &notice : std::as_const(notices)) {
        if (!notice.isWarning())
            continue;
        const QString when = notice.date.left(4);
        if (notice.kind == QLatin1StringView("expression_of_concern"))
            warnings << (when.isEmpty() ? tr("Concerns have been raised about this.") : tr("Concerns were raised about this in %1.").arg(when));
        else if (notice.kind == QLatin1StringView("withdrawal") || notice.kind == QLatin1StringView("removal"))
            warnings << (when.isEmpty() ? tr("This has been withdrawn.") : tr("This was withdrawn in %1.").arg(when));
        else if (notice.kind == QLatin1StringView("partial_retraction"))
            warnings << (when.isEmpty() ? tr("Part of this has been retracted.") : tr("Part of this was retracted in %1.").arg(when));
        else
            warnings << (when.isEmpty() ? tr("This has been retracted.") : tr("This was retracted in %1.").arg(when));
    }
    warnings.removeDuplicates();
    m_noticeWarning->setText(warnings.join(u' '));
    m_noticeWarning->setToolTip(tr("From Crossref, which lists notices published about a work. Each is listed below."));
    m_noticeWarning->setVisible(!warnings.isEmpty());

    const QString undoSource = metadata::undoableSource(m_session.database(), p.id);
    m_undoLabel->setText(tr("Details from %1.").arg(metadata::providerLabel(undoSource)));
    m_undoRow->setVisible(!undoSource.isEmpty());
    m_readStatus->setCurrentIndex(m_readStatus->findData(int(p.readStatus)));
    m_rating->setCurrentIndex(m_rating->findData(p.rating.value_or(0)));
    if (m_notes->toPlainText() != p.notes)
        m_notes->setPlainText(p.notes);
    m_notesDirty = false;

    while (m_facts->rowCount() > 0)
        m_facts->removeRow(0);
    addFact(tr("Type"), typeLabel(p.type));
    if (p.year)
        addFact(tr("Year"), QString::number(*p.year));
    addFact(tr("Journal"), p.containerTitle);

    QStringList where;
    if (!p.volume.isEmpty())
        where << tr("Volume %1").arg(p.volume);
    if (!p.issue.isEmpty())
        where << tr("Issue %1").arg(p.issue);
    if (!p.pages.isEmpty())
        where << tr("Pages %1").arg(p.pages);
    addFact(tr("Where"), where.join(QStringLiteral(", ")));

    addFact(tr("Publisher"), p.publisher);
    addFact(tr("Place"), p.place);
    addFact(tr("Edition"), p.edition);
    addFact(tr("Series"), p.series);
    addFact(tr("Conference"), p.conferenceName);
    addFact(tr("Location"), p.conferenceLocation);
    addFact(tr("Institution"), p.institution);
    addFact(tr("Department"), p.department);
    addFact(tr("Degree"), p.degree);
    addFact(tr("Report no."), p.reportNumber);
    addFact(tr("Standard"), p.standardNumber);
    addFact(tr("Issued by"), p.standardBody);
    addFact(tr("Revision"), p.standardRevision);
    addFact(tr("Status"), p.standardStatus);

    if (!p.doi.isEmpty())
        addLink(tr("DOI"), p.doi, doiLink(p.doi));
    for (const Notice &notice : std::as_const(notices)) {
        // Labels come from Crossref, so they're only ever shown as text.
        const QString text = notice.date.isEmpty() ? notice.noticeDoi : tr("%1, %2").arg(notice.date, notice.noticeDoi);
        addLink(notice.label.isEmpty() ? tr("Notice") : notice.label, text, doiLink(notice.noticeDoi));
    }
    addFact(tr("ISBN"), p.isbn13.isEmpty() ? p.isbn10 : p.isbn13);
    addFact(tr("ISSN"), p.issn.isEmpty() ? p.eissn : p.issn);
    if (!p.arxivId.isEmpty()) {
        QUrl arxiv(QStringLiteral("https://arxiv.org/"));
        arxiv.setPath(QStringLiteral("/abs/") + p.arxivId, QUrl::DecodedMode);
        addLink(tr("arXiv"), p.arxivId, arxiv);
    }
    addFact(tr("PubMed"), p.pmid);
    if (!p.url.isEmpty())
        addLink(tr("Web"), p.url, safeWebLink(p.url));
    addFact(tr("Language"), p.language);
    addFact(tr("Abstract"), p.abstract);
    if (const qint64 annotations = AnnotationRepository(m_session.database()).countFor(p.id))
        addFact(tr("Highlights and notes"), QLocale().toString(annotations));
    if (p.createdAt != 0) {
        addFact(tr("Added"),
                QLocale().toString(QDateTime::fromMSecsSinceEpoch(p.createdAt).date(), QLocale::ShortFormat));
    }

    clearLayout(m_tagRows);
    for (const Tag &tag : m_session.tags().tagsFor(p.id)) {
        auto *row = new QHBoxLayout;
        row->addWidget(plainLabel(m_session.tags().path(tag.id)), 1);
        auto *remove = new QToolButton;
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(tr("Remove this tag"));
        remove->setAccessibleName(tr("Remove tag %1").arg(tag.name));
        const qint64 tagId = tag.id;
        connect(remove, &QToolButton::clicked, this, [this, tagId] { removeTag(tagId); });
        row->addWidget(remove);
        m_tagRows->addLayout(row);
    }
    refreshTagCompleter(m_tagEdit);

    QStringList collections;
    for (const Collection &collection : m_session.collections().collectionsFor(p.id))
        collections << collection.name;
    m_collections->setText(tr("In collections: %1").arg(collections.join(QStringLiteral(", "))));
    m_collections->setVisible(!collections.isEmpty());

    const std::optional<StoredSummary> stored = SummaryRepository(m_session.database()).forPublication(p.id);
    std::optional<summary::BatchEntry> pending;
    try {
        pending = summary::SummaryBatches::entryFor(m_session.database(), p.id);
    } catch (const std::exception &) {
        // It's a help, not what's being looked at.
    }
    m_summaryPending->setVisible(pending.has_value());
    m_summaries->setVisible(pending.has_value());
    if (pending) {
        const QString sent = QLocale().toString(QDateTime::fromMSecsSinceEpoch(pending->sentAt), QLocale::ShortFormat);
        switch (pending->state) {
        case summary::BatchEntry::State::Arrived:
            m_summaryPending->setText(tr("A summary from Claude, asked for at half price, has arrived. It's kept for you to review, "
                                         "and only replaces anything here if you keep it."));
            m_summaries->setText(tr("Review summary..."));
            break;
        case summary::BatchEntry::State::Failed:
            m_summaryPending->setText(tr("A summary asked for at half price on %1 couldn't be made: %2").arg(sent, pending->problem));
            m_summaries->setText(tr("Summaries from Claude..."));
            break;
        default:
            m_summaryPending->setText(tr("A summary is on its way from Claude, asked for at half price on %1. It usually arrives "
                                         "within the hour while Papertrail is open, and waits for you to review it.")
                                          .arg(sent));
            m_summaries->setText(tr("Summaries from Claude..."));
            break;
        }
    }
    m_summaryInfo->setVisible(stored.has_value());
    m_summaryView->setVisible(stored.has_value());
    m_deleteSummary->setVisible(stored.has_value());
    m_summarize->setText(stored ? tr("Summarize again...") : tr("Summarize with Claude..."));
    if (stored) {
        const std::optional<summary::ModelInfo> model = summary::modelInfo(stored->model);
        QString info = tr("Generated by %1 on %2, from %3. It can be wrong, so check it against the paper.")
                           .arg(model ? model->label : stored->model,
                                QLocale().toString(QDateTime::fromMSecsSinceEpoch(stored->createdAt).date(), QLocale::ShortFormat),
                                stored->sentPaper ? tr("the paper you sent") : stored->basis);
        if (stored->identificationUnsure)
            info += tr(" It wasn't sure it had found the right paper.");
        m_summaryInfo->setText(info);
        QString text = stored->text;
        if (!stored->sources.isEmpty()) {
            text += QStringLiteral("\n\n") + tr("Sources") + u'\n';
            for (const auto &source : stored->sources)
                text += (source.second.isEmpty() ? source.first : source.second + QStringLiteral(" (") + source.first + u')') + u'\n';
        }
        m_summaryView->setPlainText(text.trimmed());
    }

    m_matchList->clear();
    if (!m_contentsMatch.isEmpty()) {
        try {
            for (const ContentMatch &match : FullTextIndex(m_session.database()).matchesFor(p.id, m_contentsMatch)) {
                auto *item = new QListWidgetItem(tr("Page %1: %2").arg(match.page + 1).arg(match.snippet), m_matchList);
                item->setData(Qt::UserRole, match.fileId);
                item->setData(Qt::UserRole + 1, match.page);
            }
        } catch (const std::exception &) {
            // They're a help, not what's being looked at.
        }
    }
    m_matchesSection->setVisible(m_matchList->count() > 0);

    const bool holdsArticles = !p.parentId && canHoldArticles(p.type);
    m_articlesSection->setVisible(holdsArticles);
    if (holdsArticles) {
        m_articleList->clear();
        for (const qint64 articleId : m_session.publications().childIds(p.id)) {
            const std::optional<Publication> article = m_session.publications().get(articleId);
            if (!article)
                continue;
            QString pages;
            if (article->pageStart) {
                pages = article->pageEnd && *article->pageEnd != *article->pageStart
                    ? tr("Pages %1 to %2").arg(*article->pageStart).arg(*article->pageEnd)
                    : tr("Page %1").arg(*article->pageStart);
            }
            // The item's text holds all of it, for screen readers; ArticleDelegate draws the parts apart.
            auto *item = new QListWidgetItem(pages.isEmpty() ? article->title : article->title + u'\n' + pages, m_articleList);
            item->setData(Qt::UserRole, articleId);
            item->setData(ArticleDelegate::TitleRole, article->title);
            item->setData(ArticleDelegate::PagesRole, pages);
        }
    }

    m_partOf->setVisible(p.parentId.has_value());
    m_showVolume->setVisible(p.parentId.has_value());
    m_deleteArticle->setVisible(p.parentId.has_value());
    if (p.parentId) {
        const std::optional<Publication> volume = m_session.publications().get(*p.parentId);
        if (volume)
            m_showVolume->setText(showHolderLabel(volume->type));
        QString text = tr("Part of %1.").arg(volume ? volume->title : QString());
        if (p.pageStart) {
            text += QLatin1Char(' ')
                + (p.pageEnd && *p.pageEnd != *p.pageStart ? tr("Pages %1 to %2 of its file.").arg(*p.pageStart).arg(*p.pageEnd)
                                                           : tr("Page %1 of its file.").arg(*p.pageStart));
        }
        m_partOf->setText(text);
    }

    clearLayout(m_fileRows);
    const qint64 fileOwner = p.parentId.value_or(p.id);
    const QList<FileRecord> files = m_session.files().filesFor(fileOwner);

    QStringList formats;
    for (const FileRecord &file : files) {
        if (!formats.contains(file.format))
            formats << file.format;
    }
    const bool choiceOfFormats = formats.size() > 1;

    clearLayout(m_openButtons);
    m_open->setVisible(!choiceOfFormats);
    if (choiceOfFormats) {
        QStringList offered;
        for (const FileRecord &file : files) {
            if (offered.contains(file.format) || !QFileInfo::exists(m_session.files().absolutePath(file.id)))
                continue;
            offered << file.format;
            auto *button = new QPushButton(tr("Open %1").arg(file.format.toUpper()));
            const qint64 publicationId = p.id;
            const qint64 fileId = file.id;
            connect(button, &QPushButton::clicked, this, [this, publicationId, fileId] {
                emit openFileRequested(publicationId, fileId);
            });
            m_openButtons->addWidget(button);
        }
    }

    m_coverSha.clear();
    QStringList separated;
    for (const FileRecord &file : files) {
        if (m_coverSha.isEmpty())
            m_coverSha = p.parentId && p.pageStart ? articleCoverKey(file.sha256, *p.pageStart) : file.sha256;

        QStringList facts;
        if (!file.format.isEmpty())
            facts << file.format.toUpper();
        if (file.pageCount)
            facts << (*file.pageCount == 1 ? tr("1 page") : tr("%1 pages").arg(*file.pageCount));
        if (file.hasTextLayer && !*file.hasTextLayer)
            facts << tr("no text");
        const QString problem = fileStatusLabel(
            file.status == FileStatus::Missing ? QStringLiteral("missing")
            : file.status == FileStatus::Offline ? QStringLiteral("offline")
            : file.status == FileStatus::Unreadable ? QStringLiteral("unreadable")
                                                    : QString());
        if (!problem.isEmpty())
            facts << (file.problem.isEmpty() ? problem : problem + QStringLiteral(": ") + file.problem);
        // A path has nowhere to wrap, so it scrolls instead, and copies exactly as it is.
        const QString path = QDir::toNativeSeparators(m_session.files().absolutePath(file.id));
        auto *pathField = new QLineEdit(path);
        pathField->setReadOnly(true);
        pathField->setFrame(false);
        pathField->setToolTip(path);
        pathField->setAccessibleName(tr("File location"));
        pathField->setCursorPosition(int(path.size()));
        m_fileRows->addWidget(pathField);
        m_fileRows->addWidget(plainLabel(facts.join(QStringLiteral(", "))));

        if (choiceOfFormats && !separated.contains(file.format)) {
            separated << file.format;
            auto *separate = new QPushButton(tr("Not the same publication: separate the %1").arg(file.format.toUpper()));
            separate->setToolTip(tr("Makes this format a publication of its own, with its own details."));
            const QString format = file.format;
            connect(separate, &QPushButton::clicked, this, [this, format] {
                // Removing this row's button from inside its own click would delete it too early.
                QTimer::singleShot(0, this, [this, format] {
                    if (!m_id)
                        return;
                    try {
                        separateFormat(m_session.database(), *m_id, format);
                        reload();
                        emit libraryEdited();
                    } catch (const std::exception &error) {
                        QMessageBox::warning(this, tr("Saving"), tr("That couldn't be separated: %1").arg(QString::fromUtf8(error.what())));
                    }
                });
            });
            m_fileRows->addWidget(separate);
        }
    }
    if (files.isEmpty()) {
        // One imported from another computer may be waiting for its file to turn up here.
        QString waitingFor;
        try {
            auto waiting = m_session.database().prepare("SELECT file_name FROM expected_file WHERE publication_id = ? ORDER BY is_primary DESC");
            waiting->bind(1, p.id);
            if (waiting->step())
                waitingFor = waiting->textAt(0);
        } catch (const std::exception &) {
        }
        m_fileRows->addWidget(plainLabel(waitingFor.isEmpty()
                                             ? tr("No file linked.")
                                             : tr("Waiting for its file, %1. It's connected when a file with the same contents is "
                                                  "found in one of the library's folders.")
                                                   .arg(waitingFor)));
        if (!p.parentId) {
            auto *find = new QPushButton(tr("Find its file..."));
            find->setToolTip(tr("Choose the file in one of the library's folders that is this publication."));
            const qint64 id = p.id;
            connect(find, &QPushButton::clicked, this, [this, id] { emit findFileRequested(id); });
            m_fileRows->addWidget(find);
        }
    }
    m_open->setEnabled(!files.isEmpty());
    m_showInFolder->setEnabled(!files.isEmpty());

    updateCover();
    m_loading = false;
}

void DetailsPanel::addFact(const QString &label, const QString &value)
{
    if (!value.isEmpty())
        m_facts->addRow(label, plainLabel(value));
}

void DetailsPanel::addLink(const QString &label, const QString &text, const QUrl &url)
{
    if (url.isEmpty()) {
        // Not a web address, so it's shown but can't be followed.
        addFact(label, text);
        return;
    }
    auto *link = new QLabel(linkHtml(url, text));
    link->setTextFormat(Qt::RichText);
    link->setWordWrap(true);
    link->setTextInteractionFlags(Qt::TextBrowserInteraction);
    link->setOpenExternalLinks(false);
    link->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(link, &QLabel::linkActivated, this, [](const QString &target) {
        const QUrl safe = safeWebLink(target);
        if (!safe.isEmpty())
            QDesktopServices::openUrl(safe);
    });
    m_facts->addRow(label, link);
}

void DetailsPanel::refreshTagCompleter(QLineEdit *edit)
{
    QStringList paths;
    for (const Tag &tag : m_session.tags().all())
        paths << m_session.tags().path(tag.id);
    auto *completer = new QCompleter(paths, edit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    if (QCompleter *old = edit->completer())
        old->deleteLater();
    edit->setCompleter(completer);
}

void DetailsPanel::changePublication(const std::function<void(Publication &)> &change)
{
    if (!m_id)
        return;
    try {
        std::optional<Publication> p = m_session.publications().get(*m_id);
        if (!p)
            return;
        change(*p);
        m_session.publications().update(*p);
        emit libraryEdited();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Saving"), tr("That change couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        reload();
    }
}

void DetailsPanel::undoLookup()
{
    if (!m_id)
        return;
    try {
        metadata::undoLastLookup(m_session.database(), *m_id);
        reload();
        emit libraryEdited();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Saving"), tr("That couldn't be undone: %1").arg(QString::fromUtf8(error.what())));
    }
}

void DetailsPanel::setMarkedAsPreprint(bool marked)
{
    if (!m_id)
        return;
    try {
        std::optional<Publication> p = m_session.publications().get(*m_id);
        if (!p)
            return;
        if (marked) {
            p->type = PublicationType::Preprint;
        } else {
            // Unticking puts back whatever it was before being marked.
            PublicationType earlier = PublicationType::Other;
            for (const ProvenanceEntry &entry : m_session.publications().provenance(*m_id, QStringLiteral("type"))) {
                if (entry.newValue == toDbString(PublicationType::Preprint)) {
                    earlier = publicationTypeFromDb(entry.previousValue).value_or(PublicationType::Other);
                    break;
                }
            }
            p->type = earlier;
        }
        m_session.publications().updateWithProvenance(*p, QStringLiteral("manual"));
        reload();
        emit libraryEdited();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Saving"), tr("That change couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        reload();
    }
}

void DetailsPanel::saveNotes()
{
    m_notesTimer->stop();
    if (!m_notesDirty || !m_id)
        return;
    m_notesDirty = false;
    const QString text = m_notes->toPlainText();
    changePublication([&text](Publication &p) { p.notes = text; });
}

void DetailsPanel::addTag(const QString &path)
{
    if (!m_id)
        return;
    try {
        m_session.tags().assign(*m_id, m_session.tags().ensurePath(path));
        reload();
        emit libraryEdited();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Tags"), tr("That tag couldn't be added: %1").arg(QString::fromUtf8(error.what())));
    }
}

void DetailsPanel::removeTag(qint64 tagId)
{
    if (!m_id)
        return;
    m_session.tags().unassign(*m_id, tagId);
    // Removing the row from inside its own button's click would delete the button too early.
    QTimer::singleShot(0, this, [this] {
        reload();
        emit libraryEdited();
    });
}

} // namespace papertrail::ui
