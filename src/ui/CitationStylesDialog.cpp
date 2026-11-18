#include "ui/CitationStylesDialog.h"

#include "citation/CslThread.h"
#include "citation/Styles.h"
#include "core/repo/CslStyleRepository.h"
#include "ui/CitationActions.h"
#include "ui/LibrarySession.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace citation;

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kAddedRole = Qt::UserRole + 1;

// Shown when nothing is chosen in the library, so every style has something to show.
QList<CitationItem> madeUpItems()
{
    CitationItem article;
    article.kind = CitationItem::Kind::Article;
    article.title = QStringLiteral("Lattice Vibrations in Pretend Crystals");
    article.authors = {{QStringLiteral("Example"), QStringLiteral("Ada")}, {QStringLiteral("Sample"), QStringLiteral("Ben")}};
    article.year = 2021;
    article.containerTitle = QStringLiteral("Journal of Imaginary Findings");
    article.volume = QStringLiteral("3");
    article.issue = QStringLiteral("2");
    article.pages = QStringLiteral("43-70");
    article.doi = QStringLiteral("10.5555/jif.2021.0002");

    CitationItem book;
    book.kind = CitationItem::Kind::Book;
    book.title = QStringLiteral("Imaginary Structures: A Practical Guide");
    book.authors = {{QStringLiteral("Fiction"), QStringLiteral("Dana")}};
    book.year = 2018;
    book.edition = QStringLiteral("3");
    book.publisher = QStringLiteral("Example Press");
    book.place = QStringLiteral("Bristol");
    return {article, book};
}

QString groupFor(const CslStyleInfo &style)
{
    if (style.isNote || style.citationFormat == u"note")
        return CitationStylesDialog::tr("Notes");
    if (style.citationFormat == u"author-date" || style.citationFormat == u"author")
        return CitationStylesDialog::tr("Author and date");
    if (style.citationFormat == u"numeric" || style.citationFormat == u"label")
        return CitationStylesDialog::tr("Numbered");
    return CitationStylesDialog::tr("Other");
}

} // namespace

CitationStylesDialog::CitationStylesDialog(LibrarySession &session, CitationActions &citations, QWidget *parent)
    : QDialog(parent), m_session(session), m_citations(citations)
{
    setWindowTitle(tr("Citation styles"));
    resize(900, 560);
    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("styleSearch"));
    m_search->setPlaceholderText(tr("Find a style by its name or subject"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &CitationStylesDialog::filter);
    layout->addWidget(m_search);

    auto *splitter = new QSplitter(this);
    m_list = new QTreeWidget(splitter);
    m_list->setObjectName(QStringLiteral("styleList"));
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(true);
    m_list->setUniformRowHeights(true);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] {
        updateButtons();
        updatePreview();
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, kKeyRole).toString().isEmpty())
            useChosen();
    });
    m_preview = new QTextBrowser(splitter);
    m_preview->setObjectName(QStringLiteral("stylePreview"));
    m_preview->setOpenLinks(false);
    m_preview->setOpenExternalLinks(false);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("styleStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add a style file..."), this);
    add->setObjectName(QStringLiteral("addStyleFile"));
    add->setToolTip(tr("Adds a CSL style (.csl) from this computer, such as one from the Zotero Style Repository, to this library"));
    connect(add, &QPushButton::clicked, this, [this] {
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString path = QFileDialog::getOpenFileName(this, tr("Add a citation style"), folder, tr("CSL styles (*.csl);;All files (*)"));
        if (!path.isEmpty())
            addStyleFile(path);
    });
    m_remove = new QPushButton(tr("Remove"), this);
    m_remove->setObjectName(QStringLiteral("removeStyle"));
    connect(m_remove, &QPushButton::clicked, this, &CitationStylesDialog::removeChosen);
    m_use = new QPushButton(tr("Use this style"), this);
    m_use->setObjectName(QStringLiteral("useStyle"));
    m_use->setDefault(true);
    connect(m_use, &QPushButton::clicked, this, &CitationStylesDialog::useChosen);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(add);
    buttons->addWidget(m_remove);
    buttons->addStretch(1);
    buttons->addWidget(m_use);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_citations.cslThread(), &CslThread::finished, this, &CitationStylesDialog::onRendered);
    m_items = madeUpItems();
    fill(m_citations.styleKey());
}

void CitationStylesDialog::showPreviewOf(const QList<CitationItem> &items)
{
    m_items = items.isEmpty() ? madeUpItems() : items.mid(0, kMaxPreviewed);
    updatePreview();
}

void CitationStylesDialog::fill(const QString &keep)
{
    m_list->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    const auto group = [&](const QString &name) {
        QTreeWidgetItem *&item = groups[name];
        if (!item) {
            item = new QTreeWidgetItem(m_list, {name});
            item->setFlags(Qt::ItemIsEnabled);
            QFont bold = item->font(0);
            bold.setBold(true);
            item->setFont(0, bold);
        }
        return item;
    };
    const auto add = [&](QTreeWidgetItem *parent, const QString &key, const QString &title, const QString &tip, bool added) {
        auto *item = new QTreeWidgetItem(parent, {title});
        item->setData(0, kKeyRole, key);
        item->setData(0, kAddedRole, added);
        item->setToolTip(0, tip);
        return item;
    };

    QList<core::AddedCslStyle> own;
    try {
        own = core::CslStyleRepository(m_session.database()).all();
    } catch (const std::exception &) {
    }
    if (!own.isEmpty()) {
        QTreeWidgetItem *parent = group(tr("Added to this library"));
        for (const core::AddedCslStyle &style : own)
            add(parent, cslStyleKey(style.id), style.title, style.id, true);
    }
    QTreeWidgetItem *written = group(tr("Written for Papertrail"));
    for (const CitationStyle style : allCitationStyles())
        add(written, styleKey(style), styleName(style), tr("Papertrail's own, with case law in its legal form"), false);
    // The groups in a fixed order, made before any style is put in them.
    for (const QString &name : {tr("Author and date"), tr("Numbered"), tr("Notes"), tr("Other")})
        group(name);
    for (const CslStyleInfo &style : bundledCslStyles()) {
        QString tip = style.shortTitle.isEmpty() ? style.id : style.shortTitle;
        if (!style.fields.isEmpty())
            tip += u'\n' + style.fields.join(QStringLiteral(", "));
        add(group(groupFor(style)), cslStyleKey(style.id), style.title, tip, false);
    }
    for (QTreeWidgetItem *item : std::as_const(groups)) {
        if (item->childCount() == 0)
            delete item;
    }
    m_list->expandAll();
    filter();
    choose(keep);
    updateButtons();
}

void CitationStylesDialog::filter()
{
    const QStringList words = m_search->text().simplified().split(u' ', Qt::SkipEmptyParts);
    for (int g = 0; g < m_list->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_list->topLevelItem(g);
        int shown = 0;
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem *item = group->child(i);
            const QString text = item->text(0) + u' ' + item->toolTip(0);
            const bool matches = std::all_of(words.cbegin(), words.cend(), [&](const QString &word) {
                return text.contains(word, Qt::CaseInsensitive);
            });
            item->setHidden(!matches);
            shown += matches ? 1 : 0;
        }
        group->setHidden(shown == 0);
    }
}

QString CitationStylesDialog::chosenKey() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return item ? item->data(0, kKeyRole).toString() : QString();
}

void CitationStylesDialog::choose(const QString &key)
{
    if (key.isEmpty())
        return;
    for (int g = 0; g < m_list->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_list->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            if (group->child(i)->data(0, kKeyRole).toString() == key) {
                m_list->setCurrentItem(group->child(i));
                m_list->scrollToItem(group->child(i));
                return;
            }
        }
    }
}

void CitationStylesDialog::updateButtons()
{
    const QTreeWidgetItem *item = m_list->currentItem();
    const bool style = item && !item->data(0, kKeyRole).toString().isEmpty();
    m_use->setEnabled(style);
    m_remove->setEnabled(style && item->data(0, kAddedRole).toBool());
}

void CitationStylesDialog::updatePreview()
{
    const QString key = chosenKey();
    m_previewReferences.clear();
    m_previewCitation.clear();
    m_previewError.clear();
    m_referencesRequest = 0;
    m_citationRequest = 0;
    if (key.isEmpty()) {
        m_preview->clear();
        return;
    }
    if (const std::optional<CitationStyle> builtIn = styleFromKey(key)) {
        m_previewReferences = formatReferences(m_items, *builtIn).entries;
        m_previewCitation = combinedInTextCitation(m_items, *builtIn).html();
        showPreviewText();
        return;
    }
    QString error;
    const std::optional<CslLayout> layout = m_citations.cslLayout(key, &error);
    if (!layout) {
        m_previewError = error;
        showPreviewText();
        return;
    }
    m_preview->setPlainText(tr("Working out the preview..."));
    CslThread *thread = m_citations.cslThread();
    m_referencesRequest = thread->render(QStringLiteral("preview"), CslThread::Kind::References, *layout, m_items);
    m_citationRequest = thread->render(QStringLiteral("preview citation"), CslThread::Kind::Citation, *layout, m_items);
}

void CitationStylesDialog::showPreviewText()
{
    QString html;
    if (!m_previewError.isEmpty()) {
        html = QStringLiteral("<p>") + tr("This style can't be used: %1").arg(m_previewError).toHtmlEscaped() + QStringLiteral("</p>");
    } else {
        html = QStringLiteral("<h4>") + tr("References").toHtmlEscaped() + QStringLiteral("</h4>");
        for (const RichText &entry : std::as_const(m_previewReferences))
            html += QStringLiteral("<p>") + entry.html() + QStringLiteral("</p>");
        html += QStringLiteral("<h4>") + tr("In the text").toHtmlEscaped() + QStringLiteral("</h4><p>") + m_previewCitation + QStringLiteral("</p>");
    }
    m_preview->setHtml(html);
    emit previewShown();
}

void CitationStylesDialog::onRendered(qint64 request, const QList<RichText> &entries, const QList<int> &, const QString &error)
{
    if (request == m_checkRequest) {
        m_checkRequest = 0;
        if (!error.isEmpty()) {
            const QString text = tr("%1 can't be used: %2").arg(m_checking.title, error);
            m_status->setText(text);
            emit addFinished(false, text);
            return;
        }
        try {
            core::CslStyleRepository(m_session.database()).put(m_checking.id, m_checking.title, m_checkingXml);
        } catch (const std::exception &failure) {
            const QString text = tr("%1 couldn't be added: %2").arg(m_checking.title, QString::fromUtf8(failure.what()));
            m_status->setText(text);
            emit addFinished(false, text);
            return;
        }
        fill(cslStyleKey(m_checking.id));
        const QString text = tr("Added %1 to this library. Use this style to cite in it.").arg(m_checking.title);
        m_status->setText(text);
        emit addFinished(true, text);
        return;
    }
    if (request != m_referencesRequest && request != m_citationRequest)
        return;
    if (!error.isEmpty())
        m_previewError = error;
    if (request == m_referencesRequest) {
        m_referencesRequest = 0;
        m_previewReferences = entries;
    } else {
        m_citationRequest = 0;
        m_previewCitation = entries.isEmpty() ? QString() : entries.first().html();
    }
    if (m_referencesRequest == 0 && m_citationRequest == 0)
        showPreviewText();
}

void CitationStylesDialog::addStyleFile(const QString &path)
{
    const auto fail = [this](const QString &text) {
        m_status->setText(text);
        emit addFinished(false, text);
    };
    const QString name = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(tr("%1 couldn't be read: %2").arg(name, file.errorString()));
    if (file.size() > kMaxCslStyleBytes)
        return fail(tr("%1 isn't a style: it's far larger than any style should be.").arg(name));
    const QByteArray xml = file.read(kMaxCslStyleBytes + 1);
    QString why;
    const std::optional<CslStyleInfo> info = readCslStyle(xml, &why);
    if (!info)
        return fail(tr("%1 isn't a style Papertrail can use. %2").arg(name, why));
    if (!bundledCslStyle(info->id).isEmpty())
        return fail(tr("%1 comes with Papertrail already.").arg(info->title));

    // Checked with the parent it'll be used with, or on its own, before it's kept.
    CslLayout layout;
    layout.style = *info;
    layout.xml = xml;
    layout.locale = QStringLiteral("en-US");
    if (info->isDependent()) {
        const std::optional<CslLayout> parent = m_citations.cslLayout(cslStyleKey(info->parentId), &why);
        if (!parent)
            return fail(tr("%1 uses the layout of another style, %2, which isn't here. Add its parent style first.").arg(info->title, info->parentId));
        layout.xml = parent->xml;
    }
    m_checking = *info;
    m_checkingXml = xml;
    m_status->setText(tr("Checking %1...").arg(info->title));
    m_checkRequest = m_citations.cslThread()->render(QStringLiteral("check"), CslThread::Kind::References, layout, madeUpItems());
}

void CitationStylesDialog::removeChosen()
{
    const QTreeWidgetItem *item = m_list->currentItem();
    if (!item || !item->data(0, kAddedRole).toBool())
        return;
    const QString key = chosenKey();
    const QString title = item->text(0);
    try {
        core::CslStyleRepository(m_session.database()).remove(cslStyleId(key));
        QStringList recent = m_citations.recentStyleKeys();
        recent.removeAll(key);
        m_session.settings().setValue(QStringLiteral("citation.recent_styles"), recent.join(u'\n'));
    } catch (const std::exception &failure) {
        m_status->setText(tr("%1 couldn't be removed: %2").arg(title, QString::fromUtf8(failure.what())));
        return;
    }
    QString text = tr("Removed %1 from this library.").arg(title);
    if (m_citations.styleKey() == key) {
        m_citations.setStyle(CitationStyle::Harvard);
        text += u' ' + tr("References are in Harvard again.");
    }
    fill(m_citations.styleKey());
    m_status->setText(text);
}

void CitationStylesDialog::useChosen()
{
    const QString key = chosenKey();
    if (key.isEmpty())
        return;
    m_citations.setStyleKey(key);
    m_status->setText(tr("References and citations are copied in %1.").arg(m_citations.styleTitle(key)));
}

} // namespace papertrail::ui
