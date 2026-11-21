#include "ui/LookUpPanel.h"

#include "core/db/Database.h"
#include "core/repo/FullTextIndex.h"
#include "core/settings/SettingsStore.h"
#include "metadata/Transport.h"
#include "ui/Delegates.h"
#include "ui/LibrarySession.h"

#include <QCheckBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QVBoxLayout>

namespace papertrail::ui {

using core::Mention;
using metadata::SelectionLookup;

namespace {

// Clear of ArticleDelegate's roles, which draw each entry.
enum Role { KindRole = Qt::UserRole + 16, PublicationRole, FileRole, PageRole, AnnotationRole, ThisDocumentRole, KeyRole };

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

// Text from documents and from answers is shown as it is, so none of it can become formatting or a link.
QLabel *plainLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// Smaller, in the theme's dimmed text colour, which follows the theme when it changes.
QLabel *dimmedLabel(QWidget *parent)
{
    QLabel *label = plainLabel(parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    return label;
}

// The summary's extract leaves out formulas, so an extract that stops short says there's more on the page.
QString shownExtract(const QString &extract)
{
    const QString text = extract.trimmed();
    if (text.isEmpty() || QStringLiteral(".!?…)\"”").contains(text.back()))
        return text;
    return text + QStringLiteral(" …");
}

QString titleWithYear(const Mention &mention)
{
    QString title = mention.title.simplified();
    if (title.isEmpty())
        title = LookUpPanel::tr("Untitled");
    if (mention.year)
        title += QStringLiteral(" (%1)").arg(*mention.year);
    return title;
}

// Where it's mentioned, and the words around it, shown apart by ArticleDelegate.
QString mentionWhere(const Mention &mention)
{
    const QString where = mention.thisDocument ? LookUpPanel::tr("This document") : titleWithYear(mention);
    switch (mention.kind) {
    case Mention::Kind::Page:
        return LookUpPanel::tr("%1, page %2").arg(where).arg(mention.page + 1);
    case Mention::Kind::Highlight:
        if (mention.page >= 0)
            return LookUpPanel::tr("A highlight in %1, page %2").arg(where).arg(mention.page + 1);
        return LookUpPanel::tr("A highlight in %1").arg(where);
    case Mention::Kind::Details:
        break;
    }
    return where;
}

QString mentionContext(const Mention &mention)
{
    if (mention.kind == Mention::Kind::Details)
        return LookUpPanel::tr("In its details or notes");
    return mention.snippet.simplified();
}

} // namespace

LookUpPanel::LookUpPanel(LibrarySession &session, qint64 publicationId, QWidget *parent)
    : QWidget(parent), m_session(session), m_publicationId(publicationId)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *layout = new QVBoxLayout(content);

    auto *row = new QHBoxLayout;
    m_edit = new QLineEdit(content);
    m_edit->setObjectName(QStringLiteral("lookUpWords"));
    m_edit->setPlaceholderText(tr("Words to look up"));
    m_edit->setClearButtonEnabled(true);
    row->addWidget(m_edit, 1);
    auto *go = new QPushButton(tr("Look up"), content);
    row->addWidget(go);
    layout->addLayout(row);
    connect(m_edit, &QLineEdit::returnPressed, this, [this] { lookUp(m_edit->text()); });
    connect(go, &QPushButton::clicked, this, [this] { lookUp(m_edit->text()); });

    // 1. The library.
    layout->addWidget(heading(tr("In the library"), content));
    m_libraryStatus = plainLabel(content);
    m_libraryStatus->setText(tr("Choose some words on the page, then Look up from its menu."));
    layout->addWidget(m_libraryStatus);
    m_libraryList = new QListWidget(content);
    m_libraryList->setObjectName(QStringLiteral("lookUpLibrary"));
    m_libraryList->setAccessibleName(tr("Where the library mentions it"));
    m_libraryList->setWordWrap(true);
    m_libraryList->setResizeMode(QListView::Adjust);
    m_libraryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_libraryList->setMinimumHeight(160);
    m_libraryList->setItemDelegate(new ArticleDelegate(m_libraryList));
    m_libraryList->hide();
    connect(m_libraryList, &QListWidget::itemActivated, this, &LookUpPanel::activateMention);
    connect(m_libraryList, &QListWidget::itemClicked, this, &LookUpPanel::activateMention);
    layout->addWidget(m_libraryList, 1);
    m_searchLibrary = new QPushButton(tr("Search the library for these words"), content);
    m_searchLibrary->setObjectName(QStringLiteral("lookUpSearchLibrary"));
    m_searchLibrary->hide();
    connect(m_searchLibrary, &QPushButton::clicked, this, [this] { emit searchLibraryWanted(m_words); });
    layout->addWidget(m_searchLibrary);

    // 2. Wikipedia, and 3. Wiktionary: nothing goes to either until it's asked for.
    layout->addSpacing(12);
    m_onlineStatus = plainLabel(content);
    layout->addWidget(m_onlineStatus);

    layout->addSpacing(4);
    layout->addWidget(heading(tr("Wikipedia"), content));
    m_wikipediaButton = new QPushButton(tr("Look on Wikipedia"), content);
    m_wikipediaButton->setObjectName(QStringLiteral("lookUpWikipedia"));
    connect(m_wikipediaButton, &QPushButton::clicked, this, &LookUpPanel::askWikipedia);
    layout->addWidget(m_wikipediaButton, 0, Qt::AlignLeft);
    m_wikipediaStatus = plainLabel(content);
    layout->addWidget(m_wikipediaStatus);

    // The article: its title a little larger, what it is beneath, dimmed, then its first paragraph with room around it.
    m_wikipediaTitle = plainLabel(content);
    m_wikipediaTitle->setObjectName(QStringLiteral("lookUpWikipediaTitle"));
    QFont title = m_wikipediaTitle->font();
    title.setBold(true);
    title.setPointSizeF(title.pointSizeF() * 1.15);
    m_wikipediaTitle->setFont(title);
    m_wikipediaTitle->setContentsMargins(0, 4, 0, 0);
    layout->addWidget(m_wikipediaTitle);
    m_wikipediaDescription = dimmedLabel(content);
    m_wikipediaDescription->setObjectName(QStringLiteral("lookUpWikipediaDescription"));
    layout->addWidget(m_wikipediaDescription);
    m_wikipediaExtract = plainLabel(content);
    m_wikipediaExtract->setObjectName(QStringLiteral("lookUpWikipediaExtract"));
    m_wikipediaExtract->setContentsMargins(0, 6, 0, 6);
    layout->addWidget(m_wikipediaExtract);
    m_wikipediaOpen = new QPushButton(tr("Read on Wikipedia"), content);
    connect(m_wikipediaOpen, &QPushButton::clicked, this, [this] {
        const QUrl url = metadata::wikipediaPageLink(m_wikipediaAskedLanguage, m_wikipediaKey);
        if (!url.isEmpty())
            emit openLinkWanted(url);
    });
    layout->addWidget(m_wikipediaOpen, 0, Qt::AlignLeft);

    // Other articles that matched, each its title with what it is beneath, as the library's results are drawn.
    m_wikipediaOthersHeading = dimmedLabel(content);
    m_wikipediaOthersHeading->setText(tr("Other articles"));
    m_wikipediaOthersHeading->setContentsMargins(0, 8, 0, 0);
    layout->addWidget(m_wikipediaOthersHeading);
    m_wikipediaOthers = new QListWidget(content);
    m_wikipediaOthers->setObjectName(QStringLiteral("lookUpWikipediaOthers"));
    m_wikipediaOthers->setAccessibleName(tr("Other Wikipedia articles"));
    m_wikipediaOthers->setWordWrap(true);
    m_wikipediaOthers->setResizeMode(QListView::Adjust);
    m_wikipediaOthers->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_wikipediaOthers->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_wikipediaOthers->setItemDelegate(new ArticleDelegate(m_wikipediaOthers));
    m_wikipediaOthers->installEventFilter(this);
    connect(m_wikipediaOthers, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        m_wikipediaStatus->setText(tr("Looking on Wikipedia..."));
        lookup().showWikipediaPage(item->data(KeyRole).toString(), m_wikipediaAskedLanguage);
    });
    connect(m_wikipediaOthers, &QListWidget::itemClicked, m_wikipediaOthers, &QListWidget::itemActivated);
    layout->addWidget(m_wikipediaOthers);
    m_wikipediaCredit = dimmedLabel(content);
    m_wikipediaCredit->setText(tr("From Wikipedia, under the Creative Commons Attribution-ShareAlike licence."));
    m_wikipediaCredit->setContentsMargins(0, 4, 0, 0);
    layout->addWidget(m_wikipediaCredit);

    layout->addSpacing(12);
    layout->addWidget(heading(tr("Wiktionary"), content));
    m_wiktionaryButton = new QPushButton(tr("Look in Wiktionary"), content);
    m_wiktionaryButton->setObjectName(QStringLiteral("lookUpWiktionary"));
    connect(m_wiktionaryButton, &QPushButton::clicked, this, &LookUpPanel::askWiktionary);
    layout->addWidget(m_wiktionaryButton, 0, Qt::AlignLeft);
    m_wiktionaryStatus = plainLabel(content);
    layout->addWidget(m_wiktionaryStatus);
    m_wiktionaryText = plainLabel(content);
    m_wiktionaryText->setObjectName(QStringLiteral("lookUpWiktionaryText"));
    layout->addWidget(m_wiktionaryText);
    m_wiktionaryOpen = new QPushButton(tr("Read on Wiktionary"), content);
    connect(m_wiktionaryOpen, &QPushButton::clicked, this, [this] {
        const QUrl url = metadata::wiktionaryPageLink(m_wiktionaryTerm);
        if (!url.isEmpty())
            emit openLinkWanted(url);
    });
    layout->addWidget(m_wiktionaryOpen, 0, Qt::AlignLeft);
    m_wiktionaryCredit = dimmedLabel(content);
    m_wiktionaryCredit->setText(tr("From Wiktionary, under the Creative Commons Attribution-ShareAlike licence."));
    layout->addWidget(m_wiktionaryCredit);

    m_always = new QCheckBox(tr("Always look on Wikipedia and Wiktionary too"), content);
    m_always->setObjectName(QStringLiteral("lookUpAlways"));
    try {
        m_always->setChecked(m_session.settings().value(QStringLiteral("lookup.online")).toBool());
    } catch (const std::exception &) {
    }
    connect(m_always, &QCheckBox::toggled, this, [this](bool always) {
        try {
            m_session.settings().setValue(QStringLiteral("lookup.online"), always);
        } catch (const std::exception &) {
        }
        if (always && !m_words.isEmpty() && online()) {
            if (m_wikipediaKey.isEmpty())
                askWikipedia();
            if (m_wiktionaryTerm.isEmpty())
                askWiktionary();
        }
    });
    layout->addWidget(m_always);
    layout->addStretch();

    clearOnline();
    updateOnlineState();
}

LookUpPanel::~LookUpPanel()
{
    if (m_lookup)
        m_lookup->cancel();
    if (m_thread) {
        m_thread->disconnect(this);
        m_thread->wait();
        delete m_thread;
    }
}

QString LookUpPanel::words() const
{
    return m_words;
}

bool LookUpPanel::online() const
{
    return m_session.lookupsAreOnline();
}

QString LookUpPanel::wikiLanguage() const
{
    QString chosen;
    try {
        chosen = m_session.settings().value(QStringLiteral("lookup.language")).toString();
    } catch (const std::exception &) {
    }
    if (metadata::isWikiLanguage(chosen))
        return chosen;
    // The language the system shows itself in, not its regional formats: someone reading English with dates
    // set out the Japanese way wants English Wikipedia.
    const QStringList languages = QLocale::system().uiLanguages();
    return metadata::wikiLanguageFor(languages.isEmpty() ? QLocale::system().name() : languages.first());
}

metadata::SelectionLookup &LookUpPanel::lookup()
{
    if (!m_lookup) {
        const auto &factory = m_session.paths().transportFactory;
        m_transport = factory ? factory() : std::make_unique<metadata::NetworkTransport>();
        m_lookup = std::make_unique<SelectionLookup>(*m_transport);
        connect(m_lookup.get(), &SelectionLookup::wikipediaFound, this, &LookUpPanel::showWikipedia);
        connect(m_lookup.get(), &SelectionLookup::wiktionaryFound, this, &LookUpPanel::showWiktionary);
        connect(m_lookup.get(), &SelectionLookup::nothingFound, this, [this](SelectionLookup::Source source) {
            if (source == SelectionLookup::Source::Wikipedia)
                m_wikipediaStatus->setText(tr("Wikipedia has no article for “%1”.").arg(m_words));
            else
                m_wiktionaryStatus->setText(tr("Wiktionary has no entry for “%1”.").arg(m_wiktionaryTerm));
        });
        connect(m_lookup.get(), &SelectionLookup::failed, this, [this](SelectionLookup::Source source, const QString &message) {
            if (source == SelectionLookup::Source::Wikipedia)
                m_wikipediaStatus->setText(tr("Wikipedia couldn't be asked: %1").arg(message));
            else
                m_wiktionaryStatus->setText(tr("Wiktionary couldn't be asked: %1").arg(message));
        });
    }
    return *m_lookup;
}

void LookUpPanel::lookUp(const QString &selection)
{
    const QString words = metadata::lookupWords(selection, kMaxWords, kMaxChars);
    m_edit->setText(words);
    if (words.isEmpty())
        return;
    m_words = words;
    startLookUp();
}

void LookUpPanel::startLookUp()
{
    if (m_lookup)
        m_lookup->cancel();
    clearOnline();
    updateOnlineState();
    m_libraryList->clear();
    m_libraryList->hide();
    m_searchLibrary->show();
    m_libraryStatus->setText(tr("Looking in the library..."));
    readLibrary();
    if (online() && m_always->isChecked()) {
        askWikipedia();
        askWiktionary();
    }
}

void LookUpPanel::readLibrary()
{
    if (m_thread) {
        m_again = true;
        return;
    }
    m_libraryWords = m_words;
    const QString path = m_session.paths().database;
    const QString words = m_words;
    const qint64 publication = m_publicationId;
    auto found = std::make_shared<core::Mentions>();
    auto failure = std::make_shared<QString>();
    m_thread = QThread::create([path, words, publication, found, failure] {
        try {
            core::Database db(path);
            core::attachFullText(db, core::fullTextPathFor(path));
            *found = core::mentionsOf(db, words, publication);
        } catch (const std::exception &error) {
            *failure = QString::fromUtf8(error.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, found, failure] {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (m_again || m_libraryWords != m_words) {
            m_again = false;
            readLibrary();
            return;
        }
        showMentions(*found, *failure);
    });
    m_thread->start();
}

void LookUpPanel::showMentions(const core::Mentions &mentions, const QString &failure)
{
    m_libraryList->clear();
    if (!failure.isEmpty()) {
        m_libraryStatus->setText(tr("The library couldn't be searched: %1").arg(failure));
        emit libraryShown();
        return;
    }
    if (mentions.found.isEmpty()) {
        m_libraryStatus->setText(tr("Nothing in the library mentions “%1”.").arg(m_words));
        m_libraryList->hide();
        emit libraryShown();
        return;
    }
    m_libraryStatus->setText(tr("%n publication(s) mention “%1”.", nullptr, int(mentions.publications)).arg(m_words));
    for (const Mention &mention : mentions.found) {
        // The item's text holds both parts, for screen readers and tests.
        auto *item = new QListWidgetItem(mentionWhere(mention) + u'\n' + mentionContext(mention), m_libraryList);
        item->setData(ArticleDelegate::TitleRole, mentionWhere(mention));
        item->setData(ArticleDelegate::PagesRole, mentionContext(mention));
        item->setData(KindRole, int(mention.kind));
        item->setData(PublicationRole, mention.publicationId);
        item->setData(FileRole, mention.fileId);
        item->setData(PageRole, mention.page);
        item->setData(AnnotationRole, mention.annotationId);
        item->setData(ThisDocumentRole, mention.thisDocument);
        item->setToolTip(mention.authors);
    }
    m_libraryList->show();
    emit libraryShown();
}

void LookUpPanel::activateMention(QListWidgetItem *item)
{
    if (!item)
        return;
    const auto kind = Mention::Kind(item->data(KindRole).toInt());
    const bool here = item->data(ThisDocumentRole).toBool();
    const qint64 publication = item->data(PublicationRole).toLongLong();
    switch (kind) {
    case Mention::Kind::Page:
        if (here)
            emit showPageWanted(item->data(PageRole).toInt(), m_words);
        else
            emit openPageWanted(publication, item->data(FileRole).toLongLong(), item->data(PageRole).toInt(), m_words);
        break;
    case Mention::Kind::Highlight:
        if (here)
            emit showAnnotationWanted(item->data(AnnotationRole).toLongLong());
        else
            emit openAnnotationWanted(item->data(AnnotationRole).toLongLong());
        break;
    case Mention::Kind::Details:
        if (!here)
            emit openPublicationWanted(publication);
        break;
    }
}

void LookUpPanel::updateOnlineState()
{
    const bool isOnline = online();
    m_always->setEnabled(isOnline);
    if (!isOnline) {
        m_onlineStatus->setText(tr("Papertrail is set to stay offline, so it won't look on Wikipedia or Wiktionary. "
                                   "Choose a lookup mode under Online lookups to look there too."));
        m_onlineStatus->show();
    } else {
        m_onlineStatus->setText(tr("Only the words are sent to Wikipedia and Wiktionary, and only when you ask."));
        m_onlineStatus->setVisible(!m_always->isChecked());
    }
    const bool haveWords = !m_words.isEmpty();
    m_wikipediaButton->setVisible(isOnline && haveWords && m_wikipediaAskedLanguage.isEmpty());
    const bool dictionary = haveWords && metadata::wordCount(m_words) <= kMaxDictionaryWords;
    m_wiktionaryButton->setVisible(isOnline && dictionary && m_wiktionaryTerm.isEmpty());
    if (isOnline && haveWords && !dictionary && m_wiktionaryTerm.isEmpty())
        m_wiktionaryStatus->setText(tr("Wiktionary is only asked about a word or a short phrase."));
}

void LookUpPanel::clearOnline()
{
    m_wikipediaKey.clear();
    m_wikipediaAskedLanguage.clear();
    m_wiktionaryTerm.clear();
    for (QLabel *label : {m_wikipediaStatus, m_wikipediaTitle, m_wikipediaDescription, m_wikipediaExtract,
                          m_wiktionaryStatus, m_wiktionaryText})
        label->clear();
    m_wikipediaOthers->clear();
    for (QWidget *widget : std::initializer_list<QWidget *>{m_wikipediaTitle, m_wikipediaDescription, m_wikipediaExtract,
                                                            m_wikipediaOthersHeading, m_wikipediaOthers, m_wikipediaOpen,
                                                            m_wikipediaCredit,
                                                            m_wiktionaryText, m_wiktionaryOpen, m_wiktionaryCredit})
        widget->hide();
}

void LookUpPanel::askWikipedia()
{
    // Checked again here, as the mode can change while the panel's open.
    if (m_words.isEmpty() || !online()) {
        updateOnlineState();
        return;
    }
    m_wikipediaAskedLanguage = wikiLanguage();
    m_wikipediaButton->hide();
    m_wikipediaStatus->setText(tr("Looking on Wikipedia..."));
    lookup().lookUpOnWikipedia(m_words, m_wikipediaAskedLanguage);
}

void LookUpPanel::askWiktionary()
{
    if (m_words.isEmpty() || !online() || metadata::wordCount(m_words) > kMaxDictionaryWords) {
        updateOnlineState();
        return;
    }
    m_wiktionaryTerm = m_words;
    m_wiktionaryButton->hide();
    m_wiktionaryStatus->setText(tr("Looking in Wiktionary..."));
    lookup().lookUpOnWiktionary(m_wiktionaryTerm, wikiLanguage());
}

void LookUpPanel::showWikipedia(const metadata::WikipediaSummary &summary, const QList<metadata::WikipediaPage> &others)
{
    m_wikipediaKey = summary.key;
    m_wikipediaStatus->clear();
    m_wikipediaTitle->setText(summary.title);
    m_wikipediaDescription->setText(summary.description);
    m_wikipediaExtract->setText(summary.disambiguation ? tr("“%1” can mean several things. Choose one below, "
                                                            "or read the list on Wikipedia.").arg(summary.title)
                                                       : shownExtract(summary.extract));
    m_wikipediaTitle->show();
    m_wikipediaDescription->setVisible(!summary.description.isEmpty());
    m_wikipediaExtract->show();
    m_wikipediaOpen->show();
    m_wikipediaCredit->show();
    if (!others.isEmpty()) {
        m_wikipediaOthers->clear();
        for (const metadata::WikipediaPage &page : others) {
            auto *item = new QListWidgetItem(page.title, m_wikipediaOthers);
            item->setData(KeyRole, page.key);
            item->setData(ArticleDelegate::TitleRole, page.title);
            item->setData(ArticleDelegate::PagesRole, page.description);
            item->setToolTip(page.description.isEmpty() ? page.title : page.title + u'\n' + page.description);
        }
        m_wikipediaOthersHeading->show();
        m_wikipediaOthers->show();
        m_wikipediaOthersWidth = -1;
        fitWikipediaOthers();
    }
}

// The list is as tall as its first five entries, wrapped to its width, so a few show whole without a scroll bar.
void LookUpPanel::fitWikipediaOthers()
{
    const int width = m_wikipediaOthers->viewport()->width();
    if (width == m_wikipediaOthersWidth)
        return;
    m_wikipediaOthersWidth = width;
    int height = 0;
    for (int row = 0; row < qMin(m_wikipediaOthers->count(), 5); ++row)
        height += m_wikipediaOthers->sizeHintForRow(row);
    m_wikipediaOthers->setFixedHeight(height + 2 * m_wikipediaOthers->frameWidth());
}

bool LookUpPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_wikipediaOthers && event->type() == QEvent::Resize && m_wikipediaOthers->count() > 0)
        fitWikipediaOthers();
    return QWidget::eventFilter(watched, event);
}

void LookUpPanel::showWiktionary(const QList<metadata::WiktionaryEntry> &entries)
{
    QStringList lines;
    for (const metadata::WiktionaryEntry &entry : entries.mid(0, 6)) {
        lines << tr("%1, %2").arg(entry.language, entry.partOfSpeech.toLower());
        int number = 0;
        for (const QString &definition : entry.definitions.mid(0, 5))
            lines << QStringLiteral("%1. %2").arg(++number).arg(definition);
        lines << QString();
    }
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    m_wiktionaryStatus->clear();
    m_wiktionaryText->setText(lines.join(u'\n'));
    m_wiktionaryText->show();
    m_wiktionaryOpen->show();
    m_wiktionaryCredit->show();
}

} // namespace papertrail::ui
