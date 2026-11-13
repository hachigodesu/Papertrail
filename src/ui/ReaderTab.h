#pragma once

#include "core/repo/AnnotationRepository.h"
#include "core/repo/BookmarkRepository.h"
#include "ui/ContentsPopup.h"
#include "ui/DocumentClient.h"
#include "ui/PageView.h"
#include "ui/ReferencePreview.h"

#include <QList>
#include <QRectF>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include "core/text/Sentences.h"

#include <memory>
#include <optional>

class QListWidget;
class QMenu;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

class LibrarySession;
class ThumbnailModel;

// One open document: its pages, table of contents, thumbnails and search.
class ReaderTab : public QWidget {
    Q_OBJECT

public:
    ReaderTab(LibrarySession &session, qint64 publicationId, qint64 fileId, const QString &format, const QString &path,
              const QString &convertedPath, QWidget *parent = nullptr);
    ~ReaderTab() override;

    qint64 publicationId() const { return m_publicationId; }
    qint64 fileId() const { return m_fileId; }

    // How its tab names it, short and in full.
    void setTitles(const QString &shortTitle, const QString &fullTitle);
    QString shortTitle() const { return m_shortTitle; }
    QString fullTitle() const { return m_fullTitle; }

    // Beside another document in one tab: it offers to close just this side. See ReaderPair.
    void setInPair(bool inPair);
    bool isInPair() const { return m_inPair; }
    QToolButton *closeSideButton() const { return m_closeSide; }
    // Whether it keeps its place for next time, which only one of two sides showing the same document does.
    void setSavesPlace(bool saves) { m_savesPlace = saves; }
    bool savesPlace() const { return m_savesPlace; }
    void setImageBudget(qint64 bytes);

    // Remembers the place in the document for next time.
    void saveState();

    // Puts the cursor in this document's search box.
    void focusSearch();

    // The contents over the page, searched as you type, for going to an entry or a page number.
    void showContentsPopup();
    ContentsPopup *contentsPopup() const { return m_contentsPopup; }

    // For an article in a volume's file: whether only its own pages are shown, or the whole file. Anything
    // else always shows the whole file. Opened from the library, an article shows only its own pages.
    void setArticleOnly(bool only);
    bool isArticleOnly() const { return m_articleOnly; }
    bool isArticle() const { return m_articlePages.has_value(); }
    QToolButton *wholeFileButton() const { return m_wholeFile; }
    QToolButton *forwardButton() const { return m_forwardButton; }

    // For tests.
    PageView *pageView() const { return m_view; }
    DocumentClient *client() const { return m_client; }
    QTreeWidget *contents() const { return m_contents; }
    QListView *thumbnails() const { return m_thumbnails; }
    QLineEdit *searchBox() const { return m_search; }
    QLabel *searchStatus() const { return m_searchStatus; }
    QLabel *problemLabel() const { return m_problem; }
    QComboBox *layoutBox() const { return m_layoutBox; }
    QComboBox *coloursBox() const { return m_coloursBox; }
    QComboBox *zoomBox() const { return m_zoomBox; }
    QLineEdit *pageBox() const { return m_pageBox; }
    bool isReady() const { return m_ready; }

    // The reading ruler, which dims all but the line or paragraph being read (reader.ruler), moved with Up and Down or
    // a click, and following on to the next page.
    void setRulerShown(bool shown);
    bool isRulerShown() const { return m_rulerShown; }
    void moveRuler(int step);

    // Reading aloud, from the start of the selection, else the ruler's line, else the top of the page being read, a
    // sentence at a time, marking each, and on through the pages shown.
    void startReadingAloud();
    // From the sentence at this point of a page, as from the page's menu, or a click while reading.
    void startReadingAloudAt(int page, const QPointF &point);
    void pauseOrResumeReadingAloud();
    void stopReadingAloud();
    void nextSentence();
    void previousSentence();
    class ReadAloudBar *readAloudBar() const { return m_bar; }
    bool isReadingAloud() const { return m_reading; }
    bool isReadingPaused() const;
    // Where it's got to, for tests: the page, and which sentence of those on it.
    int readingPage() const { return m_reading ? m_readingPage : -1; }
    int readingSentence() const { return m_readingIndex; }
    // The lines of a page read so far, as the ruler and reading aloud have them.
    QList<ReaderTextLine> pageLines(int page) const { return m_pageLines.value(page); }
    // Where it is, for tests: the page, and which line or paragraph of it.
    int rulerPage() const { return m_rulerPage; }
    int rulerIndex() const { return m_rulerIndex; }
    // The parts of a page the ruler stops at, with their text, once its lines are known.
    struct RulerStop {
        QRectF rect;
        QString text;
        bool figure = false; // A whole figure, which nothing joins.
        // Which table it is, all of whose rows it is, or -1.
        int table = -1;
    };
    QList<RulerStop> rulerStops(int page) const;
    bool isShowingProblem() const;
    int hitCount() const { return int(m_hits.size()); }
    int currentHitIndex() const { return m_currentHit; }
    bool isReflowable() const { return m_reflowable; }
    double fontSize() const { return m_layout.fontSize; }
    void changeFontSize(double change);
    // Turns the pages clockwise by this much, kept with the place. Only fixed pages can be turned.
    void rotateBy(int degrees);

    // Recognises the page being read whatever text it has, and from then on uses the document's recognised words in
    // place of its own, for a document whose own text is poor. False if this reader can't recognise text.
    bool recogniseEveryPage();
    // Reads the recognised words again, after the background has recognised more pages.
    void rereadRecognisedWords();
    const QString &contentHash() const { return m_contentHash; }

    // Keeps the place at the top of the view as a bookmark, named after its page.
    void addBookmark();
    void goToBookmark(qint64 id);
    QListWidget *bookmarkList() const { return m_bookmarkList; }
    // The page each bookmark is on now, by id. A reflowable document's are found again after each change of layout.
    int bookmarkPage(qint64 id) const;
    void setSidePanelVisible(bool visible);
    // Full screen: leaves only the pages, and puts the toolbar and side panel back as they were afterwards.
    void setChromeHidden(bool hidden);
    bool isChromeHidden() const { return m_chromeHidden; }
    void followExternalLink(const QString &uri);
    void setOpenLinkHandler(const std::function<void(const QUrl &)> &handler) { m_openLink = handler; }
    void nextHit();
    void previousHit();
    void applySearch();

    // Goes to where a search across the library found something, and looks for it here too. For a file
    // whose pages move with its layout, the page can't be relied on, so it goes to the first match instead.
    void showMatch(int page, const QString &text);
    // Goes to a highlight or note, once the document is ready if it isn't yet.
    void showAnnotation(qint64 id);
    // Reads this document's highlights and notes again, after a change made elsewhere.
    void reloadAnnotations() { loadAnnotations(); }
    // This document's highlights and notes, as last read. For tests.
    const QList<core::Annotation> &annotations() const { return m_annotations; }

    // Highlights and notes. These are kept in the library's database, and never in the file.
    void annotateSelection(core::AnnotationType type, const QString &note = QString(), const QString &colour = QString());
    // An area drawn over a page, in points, such as a figure or an equation, kept like a highlight. Only where pages are
    // fixed. The words inside it are kept with it, so searching finds it.
    void annotateArea(int page, const QRectF &rect, const QString &note = QString(), const QString &colour = QString());
    // What's offered once an area's been drawn, for tests too.
    QMenu *areaMenu(int page, const QRectF &rect, QWidget *parent);
    // Puts a picture of an area on the clipboard, once it's made.
    void copyAreaPicture(qint64 id);

    // What the words at a point on a page refer to, such as "Figure 3", shown in a preview to go to from. As Ctrl and a
    // click does it.
    void showReferenceAt(int page, const QPointF &point, const QPoint &viewportPoint);
    ReferencePreview *referencePreview() const { return m_referencePreview; }
    void setAnnotationNote(qint64 id, const QString &note);
    void setAnnotationColour(qint64 id, const QString &colour);
    void deleteAnnotation(qint64 id);
    // `only`, if given, limits it to those highlights and notes.
    bool exportAnnotations(const QString &path, const QList<qint64> &only = {});
    QListWidget *annotationList() const { return m_annotationList; }

    // Several chosen in the Notes list at once. Each change is made in one go, and the choice is kept afterwards.
    QList<qint64> chosenAnnotations() const; // In the list's order.
    void setAnnotationsColour(const QList<qint64> &ids, const QString &colour);
    void deleteAnnotations(const QList<qint64> &ids);
    // Their words and notes as plain text, in the list's order, as copying puts them on the clipboard.
    QString annotationsAsText(const QList<qint64> &ids) const;
    void copyAnnotations(const QList<qint64> &ids) const;
    // What right-clicking the Notes list offers for what's chosen there.
    void addChosenAnnotationActions(QMenu *menu);
    QList<int> markedPages() const;

signals:
    // The user asked to open this file with something else, because it can't be shown here.
    void openElsewhereRequested(const QString &path);
    void rulerShownChanged(bool shown);
    void readingAloudChanged(bool reading);

    // Reports what's happening with this tab's search and problems, for the window's status bar.
    void statusMessage(const QString &message);

    // A highlight or note has been added, changed or removed.
    void annotationsChanged();

    // Close this side of two documents side by side.
    void closeSideRequested();

    // Escape was pressed on the pages with nothing there for it to stop, as full screen uses to leave.
    void escapeUnused();

    // A scanned page's words were recognised, so the document's text in the index is out of date.
    void textRecognised(const QString &contentHash);
    // Recognise this document's text was chosen. The window asks for the rest of it to be recognised.
    void recogniseDocumentWanted();

private:
    struct Hit {
        int page = 0;
        QRectF rect;
    };

    void buildToolbar();
    void buildSidePanel();
    void connectClient();
    void onOpened(int pageCount, bool reflowable);
    void showProblem(DocumentClient::Failure failure, const QString &message);
    void updatePageBox();
    void updateZoomBox();
    void restorePlace();
    QImage turned(const QImage &picture) const; // A picture of part of a page, turned as the pages are.
    void loadBookmarks();
    void locateBookmarks();
    void refreshBookmarkList();
    void deleteChosenBookmark();
    void onOutline(const QList<papertrail::ui::ReaderOutlineItem> &items);
    void onSearchHits(int page, const QList<QRectF> &rects);
    void updateSearchStatus();
    void showCurrentHit();
    void onContentsActivated(QTreeWidgetItem *item);
    void applyArticleRange();
    void ensurePageShown(int page);
    bool pageIsShown(int page) const;

    LibrarySession &m_session;
    void applyPendingMatch();
    void loadAnnotations();
    void locateAnnotations();
    void showMark(const core::Annotation &annotation);
    void refreshAnnotationList();
    void goToAnnotation(qint64 id);
    void onAnchored(qint64 token, int part, const QString &place, int occurrence);
    void onLocated(qint64 token, int part, int page, const QList<QRectF> &rects);
    void addAnnotationActions(QMenu *menu, const QPoint &viewportPoint);
    static QStringList colourNames();

public:
    // Adds a menu for a highlight's colour, with its current one ticked. For tests too.
    QMenu *addColourMenu(QMenu *menu, qint64 id);

private:
    void askForNoteAndAnnotate();
    void onAreaChosen(int page, const QRectF &rect, const QPoint &viewportPoint);
    void onReference(const papertrail::ui::ReaderReference &reference);
    void showReferencePreview(const QImage &picture);
    void onAreaText(qint64 token, int page, const QString &text);
    void onAreaPicture(qint64 token, int page, const QImage &image);
    void requestAreaPictures();
    void askToEditNote(qint64 id);
    void askToExportAnnotations(const QList<qint64> &only = {});
    void askToDeleteAnnotations(const QList<qint64> &ids);
    QMenu *addColourMenu(QMenu *menu, const QList<qint64> &ids);

    std::optional<std::pair<int, QString>> m_pendingMatch;
    std::optional<qint64> m_pendingAnnotation;

    // What's on this file's contents, and what's on other copies or formats of this publication.
    QString m_contentHash;
    QList<core::Annotation> m_annotations;
    QList<core::Annotation> m_annotationsElsewhere;
    QHash<qint64, QList<QPair<int, QList<QRectF>>>> m_locatedParts; // By annotation and part, in text that moves.
    qint64 m_locateRound = 0; // Answers from before the layout last changed are out of date.

    struct DraftAnnotation {
        core::Annotation annotation;
        int waitingFor = 0;
    };
    QHash<qint64, DraftAnnotation> m_drafts;
    qint64 m_nextDraft = 1;
    QHash<qint64, core::Annotation> m_areaDrafts; // Areas waiting for the words inside them, by draft.
    QHash<qint64, QImage> m_areaPictures;         // For the Notes panel, by annotation.
    QSet<qint64> m_areaPicturesAsked;
    std::optional<ReaderReference> m_wantedReference; // Found, and waiting for its picture.
    QPoint m_referencePoint;
    QPointer<ReferencePreview> m_referencePreview;

    QWidget *m_annotationPanel = nullptr;
    QListWidget *m_annotationList = nullptr;

    qint64 m_publicationId;
    qint64 m_fileId;
    QString m_shortTitle;
    QString m_fullTitle;
    bool m_inPair = false;
    bool m_savesPlace = true;
    QToolButton *m_closeSide = nullptr;
    QAction *m_closeSideAction = nullptr;

    // An article's first and last pages in its volume's file, counting from 0.
    std::optional<std::pair<int, int>> m_articlePages;
    bool m_articleOnly = false;
    QString m_wholeFileLabel;
    QToolButton *m_wholeFile = nullptr;
    QAction *m_wholeFileAction = nullptr;
    QString m_format;
    QString m_path;
    ReaderLayout m_layout;
    DocumentClient *m_client = nullptr;
    PageView *m_view = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_problem = nullptr;
    QPushButton *m_openElsewhere = nullptr;

    QToolBar *m_toolbar = nullptr;
    QLineEdit *m_pageBox = nullptr;
    QLabel *m_pageTotal = nullptr;
    QComboBox *m_zoomBox = nullptr;
    QComboBox *m_layoutBox = nullptr;
    QComboBox *m_coloursBox = nullptr;
    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;
    QToolButton *m_smallerText = nullptr;
    QToolButton *m_largerText = nullptr;
    QAction *m_smallerTextAction = nullptr;
    QAction *m_largerTextAction = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_searchStatus = nullptr;
    QToolButton *m_searchPrevious = nullptr;
    QToolButton *m_searchNext = nullptr;

    QSplitter *m_splitter = nullptr;
    QTabWidget *m_side = nullptr;
    QListWidget *m_bookmarkList = nullptr;
    QList<core::Bookmark> m_bookmarks;
    QHash<qint64, int> m_bookmarkPages; // Where each was found, for a reflowable document.
    qint64 m_bookmarkRound = 0;
    bool m_chromeHidden = false;
    bool m_sideShownBeforeHiding = false;
    QTreeWidget *m_contents = nullptr;
    QPointer<ContentsPopup> m_contentsPopup;
    QListView *m_thumbnails = nullptr;
    ThumbnailModel *m_thumbnailModel = nullptr;
    QToolButton *m_sideToggle = nullptr;

    QTimer *m_saveTimer = nullptr;
    QTimer *m_searchTimer = nullptr;

    bool m_ready = false;
    bool m_reflowable = false;
    bool m_failedToOpen = false;
    QString m_bookmark;
    double m_reached = 0; // The furthest it's been read, from 0 to 1, kept with the place.
    int m_bookmarkPage = -1;
    bool m_restoring = false;

    // Each page's lines, as the ruler needs them, until the layout changes.
    void onLines(int page, const QList<ReaderTextLine> &lines);
    void placeRuler(int page, int index);
    void rulerToPoint(int page, const QPointF &point);
    void askForLines(int page);
    QHash<int, QList<ReaderTextLine>> m_pageLines;
    QSet<int> m_linesAsked;
    bool m_layoutPending = false; // Lines that arrive meanwhile are the old layout's.
    bool m_rulerShown = false;
    int m_rulerPage = -1;
    int m_rulerIndex = 0;
    // What to do once the lines of m_rulerPage arrive: go to the first stop (1), the last (-1), or near a point.
    int m_rulerWaiting = 0;
    std::optional<QPointF> m_rulerWaitingPoint;
    QString m_rulerText; // Where it was, to find again after a new layout.

    // Where on a page to start: a point, the top of the page down to a height, or its last sentence.
    struct ReadingStart {
        double fromY = 0;
        std::optional<QPointF> at;
        bool fromEnd = false;
        bool carriedOver = false; // Its first sentence was said with the last of the page before.
    };
    bool ensureSpeaker();
    void beginReading(int page, const ReadingStart &start);
    void readPage(int page, const ReadingStart &start);
    // Reads a page once its lines, and the next page's, are here.
    void readWhenReady(int page, const ReadingStart &start);
    bool linesReadyFor(int page) const;
    void sayFrom(int index);
    void onSentenceStarted(int index);
    // Marks a sentence of the page being read as the one being read, and follows it.
    void showReading(int sentence);
    QList<core::Sentence> sentencesOfPage(int page) const;
    // Tells the speaker how the next page starts, once its lines are here, so a voice can make it ahead.
    void prepareNextPage();
    void onReadingFinished();
    void updateReadingBar();
    std::unique_ptr<class Speaker> m_speaker;
    bool m_naturalUnavailable = false; // The natural voice couldn't work here, so the system's reads.
    QString m_speakerChoice;           // Which voice m_speaker is.
    bool m_reading = false;
    int m_readingPage = -1;
    ReadingStart m_readingStart;
    bool m_readingWaiting = false;          // For the page's lines.
    QList<core::Sentence> m_pageSentences;  // All of those on the page being read.
    // The page's last sentence carries on over the page, into the next page's first, which is said with it.
    bool m_carriesOn = false;
    core::Sentence m_nextFirst;
    bool m_carriedOver = false;             // Its first sentence was said with the page before's last.
    int m_sayFrom = 0;                      // Which of them was first in what the speaker was last given.
    int m_readingIndex = -1;                // Which of them is being said.
    bool m_followReading = true;            // Until the user scrolls away from it.
    class ReadAloudBar *m_bar = nullptr;
    class QToolButton *m_readAloudButton = nullptr;
    std::function<void(const QUrl &)> m_openLink;

    QList<Hit> m_hits;
    int m_currentHit = -1;
    bool m_searchRunning = false;
    QString m_activeSearch;
    int m_searchStartPage = 0;
};

} // namespace papertrail::ui
