#include "ui/VolumeDialogs.h"

#include "core/model/Identifiers.h"
#include "core/model/PublicationFields.h"
#include "core/repo/FileRepository.h"
#include "import/ImportService.h"
#include "ui/LibrarySession.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace core;

namespace {

enum Column { TitleColumn, FirstColumn, LastColumn, NoteColumn };

} // namespace

ContentsDialog::ContentsDialog(LibrarySession &session, qint64 volumeId, QWidget *parent)
    : QDialog(parent), m_session(session), m_volumeId(volumeId)
{
    setWindowTitle(tr("Create articles from the table of contents"));
    resize(760, 520);

    auto *layout = new QVBoxLayout(this);
    m_message = new QLabel(tr("Reading the table of contents..."), this);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);

    auto *levelRow = new QHBoxLayout;
    auto *levelLabel = new QLabel(tr("Use this level of the contents:"), this);
    m_level = new QComboBox(this);
    m_level->setAccessibleName(tr("Level of the contents"));
    levelRow->addWidget(levelLabel);
    levelRow->addWidget(m_level);
    levelRow->addStretch();
    layout->addLayout(levelRow);
    levelLabel->hide();
    m_level->hide();
    connect(m_level, &QComboBox::currentIndexChanged, this, [this, levelLabel] {
        levelLabel->setVisible(m_level->count() > 1);
        if (m_level->count() > 0)
            fillTable();
    });

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("Title"), tr("First page"), tr("Last page"), QString()});
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(TitleColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(FirstColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(LastColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(NoteColumn, QHeaderView::ResizeToContents);
    connect(m_table, &QTableWidget::itemChanged, this, &ContentsDialog::updateCreateButton);
    layout->addWidget(m_table, 1);

    auto *note = new QLabel(tr("Pages are positions in the file, which may differ from the numbers printed on them. "
                               "Each article is marked as needing checking, because a table of contents only gives its title."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(this);
    m_create = buttons->addButton(tr("Create articles"), QDialogButtonBox::AcceptRole);
    m_create->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_create, &QPushButton::clicked, this, [this] {
        if (createChosen())
            accept();
    });
    layout->addWidget(buttons);

    QList<FileRecord> files;
    try {
        files = m_session.files().filesFor(m_volumeId);
    } catch (const std::exception &) {
        // Handled just below, the same as having no file.
    }
    // A reader needs one fixed layout to go by, and the first with fixed pages gives the most reliable one.
    const FileRecord *file = nullptr;
    for (const FileRecord &candidate : std::as_const(files)) {
        if (candidate.format == QLatin1StringView("pdf") || candidate.format == QLatin1StringView("djvu")
            || candidate.format == QLatin1StringView("xps")) {
            file = &candidate;
            break;
        }
    }
    if (!file && !files.isEmpty())
        file = &files.first();
    if (!file) {
        QMetaObject::invokeMethod(this, [this] { cantOffer(tr("This publication has no file to read.")); }, Qt::QueuedConnection);
        return;
    }

    m_client = new DocumentClient(m_session.paths().workerPath, this);
    connect(m_client, &DocumentClient::opened, this, &ContentsDialog::onOpened);
    connect(m_client, &DocumentClient::outlineReady, this, &ContentsDialog::onOutline);
    connect(m_client, &DocumentClient::headingReady, this, &ContentsDialog::onHeading);
    connect(m_client, &DocumentClient::failed, this,
            [this](DocumentClient::Failure, const QString &message) { cantOffer(tr("This file couldn't be read: %1").arg(message)); });
    const QString converted = file->sha256.isEmpty() ? QString() : import::convertedPathFor(m_session.paths().cacheDirectory, file->sha256);
    m_client->open(m_session.files().absolutePath(file->id), ReaderLayout{}, converted);
}

void ContentsDialog::cantOffer(const QString &message)
{
    m_message->setText(message);
    m_table->setEnabled(false);
    m_create->setEnabled(false);
    emit contentsUnavailable();
}

void ContentsDialog::onOpened(int pageCount, bool reflowable)
{
    if (reflowable) {
        // Its pages change with the size of the window and text, so a range of them wouldn't mean anything.
        cantOffer(tr("This format has no fixed pages, so articles can't be marked by page range."));
        return;
    }
    m_pageCount = pageCount;
    m_client->requestOutline();
}

void ContentsDialog::onOutline(const QList<ReaderOutlineItem> &items)
{
    m_entries.clear();
    QSet<int> depths;
    for (const ReaderOutlineItem &item : items) {
        m_entries << ContentsEntry{item.title, item.depth, item.page};
        if (item.page >= 0)
            depths.insert(item.depth);
    }
    if (depths.isEmpty()) {
        cantOffer(tr("This file has no table of contents to work from. You can still add articles by hand."));
        return;
    }

    QList<int> ordered(depths.cbegin(), depths.cend());
    std::sort(ordered.begin(), ordered.end());
    const int likely = likelyArticleDepth(m_entries);
    {
        const QSignalBlocker blocker(m_level);
        m_level->clear();
        for (const int depth : ordered) {
            int count = 0;
            for (const ContentsEntry &entry : std::as_const(m_entries))
                count += entry.depth == depth && entry.page >= 0 ? 1 : 0;
            m_level->addItem(tr("Level %1 (%2 entries)").arg(depth + 1).arg(count), depth);
        }
        m_level->setCurrentIndex(int(ordered.indexOf(likely)));
    }
    m_level->setVisible(ordered.size() > 1);
    if (auto *label = qobject_cast<QHBoxLayout *>(m_level->parentWidget()->layout()->itemAt(1)->layout())) {
        if (QWidget *levelLabel = label->itemAt(0)->widget())
            levelLabel->setVisible(ordered.size() > 1);
    }
    fillTable();
    emit contentsReady();
}

void ContentsDialog::requestHeadings()
{
    // Only for what the contents didn't name, which saves reading every article's first page, and the first
    // few for the volume's numbers, which are usually on its cover or in a running header.
    for (qsizetype i = 0; i < m_drafts.size(); ++i) {
        const ArticleDraft &draft = m_drafts.at(i);
        const int page = draft.firstPage - 1;
        const bool forNumbers = i < 3 && !m_numberingRead;
        if ((forNumbers || isPlaceholderTitle(draft.title)) && !m_headings.contains(page) && !m_headingsWanted.contains(page)) {
            m_headingsWanted.insert(page);
            m_client->requestHeading(page);
        }
    }
}

void ContentsDialog::onHeading(int page, const ReaderHeading &heading)
{
    if (!m_headingsWanted.remove(page))
        return;
    const QString &title = heading.title;
    const QString &doi = heading.doi;
    m_headings.insert(page, Heading{title, doi});

    // The numbers fill gaps in the volume's details, and its articles', changing nothing already there.
    if (!m_numberingRead && !heading.volume.isEmpty()) {
        m_numberingRead = true;
        try {
            const std::optional<int> month = heading.month > 0 ? std::optional<int>(heading.month) : std::nullopt;
            const std::optional<int> year = heading.year > 0 ? std::optional<int>(heading.year) : std::nullopt;
            if (fillVolumeNumbering(m_session.database(), m_volumeId, heading.volume, heading.issue, month, year)) {
                m_filledNumbering = true;
                m_numberingNote = tr("Its volume and issue numbers, %1 and %2, were read from its pages and filled in.")
                                      .arg(heading.volume, heading.issue);
            }
        } catch (const std::exception &) {
            // Only a convenience: they can still be typed in.
        }
    }
    for (int row = 0; row < m_drafts.size(); ++row) {
        ArticleDraft &draft = m_drafts[row];
        QTableWidgetItem *titleItem = m_table->item(row, TitleColumn);
        // A title someone has already typed over is theirs.
        if (draft.firstPage - 1 != page || !titleItem || titleItem->text().simplified() != draft.title)
            continue;
        if (adoptPageHeading(draft, title, doi))
            showRow(row);
    }
    updateCreateButton();
    updateMessage();
    if (m_headingsWanted.isEmpty())
        emit headingsRead();
}

void ContentsDialog::updateMessage()
{
    QString text = tr("Tick the entries that are articles, and correct anything that's wrong. "
                      "Nothing is created until you choose Create articles.");
    if (!m_headingsWanted.isEmpty()) {
        text += u' ' + tr("Reading titles and numbers from the articles' first pages (%n left).", nullptr,
                          int(m_headingsWanted.size()));
    }
    if (!m_numberingNote.isEmpty())
        text += u' ' + m_numberingNote;
    m_message->setText(text);
}

void ContentsDialog::fillTable()
{
    m_drafts = draftsFromContents(m_entries, m_level->currentData().toInt(), m_pageCount);
    try {
        markExistingArticles(m_session.database(), m_volumeId, m_drafts);
    } catch (const std::exception &) {
        // Only a convenience: at worst, something can be chosen twice.
    }
    for (ArticleDraft &draft : m_drafts) {
        const auto known = m_headings.constFind(draft.firstPage - 1);
        if (known != m_headings.cend())
            adoptPageHeading(draft, known->title, known->doi);
    }

    {
        const QSignalBlocker blocker(m_table);
        m_table->setRowCount(int(m_drafts.size()));
        for (int row = 0; row < m_drafts.size(); ++row) {
            auto *title = new QTableWidgetItem; // Shown as it is, so a file's text can't become formatting.
            title->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            m_table->setItem(row, TitleColumn, title);

            auto *first = new QTableWidgetItem;
            first->setData(Qt::EditRole, m_drafts.at(row).firstPage);
            m_table->setItem(row, FirstColumn, first);
            auto *last = new QTableWidgetItem;
            last->setData(Qt::EditRole, m_drafts.at(row).lastPage);
            m_table->setItem(row, LastColumn, last);

            auto *note = new QTableWidgetItem;
            note->setFlags(Qt::ItemIsEnabled);
            m_table->setItem(row, NoteColumn, note);
            showRow(row);
        }
    }
    requestHeadings();
    updateMessage();
    updateCreateButton();
}

void ContentsDialog::showRow(int row)
{
    const ArticleDraft &draft = m_drafts.at(row);
    {
        const QSignalBlocker blocker(m_table);
        QTableWidgetItem *title = m_table->item(row, TitleColumn);
        title->setText(draft.title);
        title->setCheckState(draft.likelyNotAnArticle || draft.alreadyAdded ? Qt::Unchecked : Qt::Checked);

        QStringList notes;
        if (draft.alreadyAdded)
            notes << tr("Already added");
        else if (draft.renames)
            notes << tr("Names the article already made");
        if (draft.likelyNotAnArticle)
            notes << tr("Probably not an article");
        if (draft.titleFromPage)
            notes << tr("Title from its first page");
        m_table->item(row, NoteColumn)->setText(notes.join(QStringLiteral(". ")));
    }
}

QList<ArticleDraft> ContentsDialog::chosenDrafts(QString *problem) const
{
    QList<ArticleDraft> drafts;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, TitleColumn)->checkState() != Qt::Checked)
            continue;
        ArticleDraft draft;
        draft.title = m_table->item(row, TitleColumn)->text().simplified();
        draft.firstPage = m_table->item(row, FirstColumn)->data(Qt::EditRole).toInt();
        draft.lastPage = m_table->item(row, LastColumn)->data(Qt::EditRole).toInt();
        if (row < m_drafts.size()) {
            const ArticleDraft &proposed = m_drafts.at(row);
            draft.renames = proposed.renames;
            // A DOI read from the first page only belongs to the article if it still starts there.
            if (draft.firstPage == proposed.firstPage)
                draft.doi = proposed.doi;
        }
        if (problem && problem->isEmpty()) {
            if (draft.title.isEmpty()) {
                *problem = tr("Row %1 has no title.").arg(row + 1);
            } else if (draft.firstPage < 1 || draft.lastPage < draft.firstPage || draft.lastPage > m_pageCount) {
                *problem = tr("The pages on row %1 don't make sense. This file has %2 pages.").arg(row + 1).arg(m_pageCount);
            }
        }
        drafts << draft;
    }
    return drafts;
}

bool ContentsDialog::createChosen()
{
    QString problem;
    const QList<ArticleDraft> drafts = chosenDrafts(&problem);
    if (!problem.isEmpty()) {
        m_message->setText(problem);
        return false;
    }
    try {
        const auto named = std::count_if(drafts.cbegin(), drafts.cend(), [](const ArticleDraft &draft) { return draft.renames.has_value(); });
        m_articleIds = createArticles(m_session.database(), m_volumeId, drafts, true);
        m_created = int(m_articleIds.size() - named);
        m_named = int(named);
    } catch (const std::exception &error) {
        m_message->setText(tr("Those articles couldn't be created: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
    return true;
}

void ContentsDialog::updateCreateButton()
{
    const QList<ArticleDraft> drafts = chosenDrafts();
    const auto renaming = std::count_if(drafts.cbegin(), drafts.cend(), [](const ArticleDraft &draft) { return draft.renames.has_value(); });
    const int creating = int(drafts.size() - renaming);
    m_create->setEnabled(!drafts.isEmpty());
    if (renaming == 0)
        m_create->setText(creating == 1 ? tr("Create 1 article") : tr("Create %1 articles").arg(creating));
    else if (creating == 0)
        m_create->setText(renaming == 1 ? tr("Name 1 article") : tr("Name %1 articles").arg(renaming));
    else
        m_create->setText(tr("Create %1, name %2").arg(creating).arg(renaming));
}

AddArticleDialog::AddArticleDialog(const std::optional<int> &pageCount, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Add an article"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_title = new QLineEdit(this);
    form->addRow(tr("Title"), m_title);

    m_authors = new QLineEdit(this);
    m_authors->setPlaceholderText(tr("Family, Given; Family, Given"));
    form->addRow(tr("Authors"), m_authors);

    m_doi = new QLineEdit(this);
    form->addRow(tr("DOI"), m_doi);

    const int most = pageCount.value_or(99999);
    m_first = new QSpinBox(this);
    m_first->setRange(1, most);
    form->addRow(tr("First page in the file"), m_first);
    m_last = new QSpinBox(this);
    m_last->setRange(1, most);
    form->addRow(tr("Last page in the file"), m_last);
    connect(m_first, &QSpinBox::valueChanged, this, [this](int first) {
        if (m_last->value() < first)
            m_last->setValue(first);
    });
    connect(m_last, &QSpinBox::valueChanged, this, [this](int last) {
        if (m_first->value() > last)
            m_first->setValue(last);
    });
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(this);
    m_add = buttons->addButton(tr("Add article"), QDialogButtonBox::AcceptRole);
    m_add->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_title, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_add->setEnabled(!text.trimmed().isEmpty()); });
    layout->addWidget(buttons);
}

ArticleDraft AddArticleDialog::draft() const
{
    ArticleDraft article;
    article.title = m_title->text().simplified();
    article.firstPage = m_first->value();
    article.lastPage = m_last->value();
    article.authors = peopleFromText(m_authors->text());
    article.doi = m_doi->text().trimmed();
    return article;
}

} // namespace papertrail::ui
