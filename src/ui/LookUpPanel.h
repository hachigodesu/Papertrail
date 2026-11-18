#pragma once

#include "core/query/LibraryMentions.h"
#include "metadata/SelectionLookup.h"

#include <QUrl>
#include <QWidget>

#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QThread;

namespace papertrail::metadata {
class Transport;
}

namespace papertrail::ui {

class LibrarySession;

// The reader's Look up page: what the library has about some words, then, only when asked and with lookups online,
// what Wikipedia and Wiktionary say. Only the words go online, never anything about the document.
class LookUpPanel : public QWidget {
    Q_OBJECT

public:
    LookUpPanel(LibrarySession &session, qint64 publicationId, QWidget *parent = nullptr);
    ~LookUpPanel() override;

    // Looks up a selection's words: in the library at once, and online too if that's always wanted.
    void lookUp(const QString &selection);
    QString words() const;

    // The longest selection looked up, in words and characters.
    static constexpr int kMaxWords = 8;
    static constexpr int kMaxChars = 100;
    // Wiktionary is only asked about a word or a short phrase.
    static constexpr int kMaxDictionaryWords = 3;

signals:
    // A page of this document, to go to.
    void showPageWanted(int page, const QString &text);
    void showAnnotationWanted(qint64 annotationId);
    // Somewhere else in the library.
    void openPageWanted(qint64 publicationId, qint64 fileId, int page, const QString &text);
    void openAnnotationWanted(qint64 annotationId);
    void openPublicationWanted(qint64 publicationId);
    void searchLibraryWanted(const QString &words);
    // An address built here, never one from an answer.
    void openLinkWanted(const QUrl &url);
    // The library's part has been shown, for tests.
    void libraryShown();

private:
    void startLookUp();
    void readLibrary();
    void showMentions(const core::Mentions &mentions, const QString &failure);
    void activateMention(QListWidgetItem *item);
    void updateOnlineState();
    bool online() const;
    QString wikiLanguage() const;
    metadata::SelectionLookup &lookup();
    void askWikipedia();
    void askWiktionary();
    void showWikipedia(const metadata::WikipediaSummary &summary, const QList<metadata::WikipediaPage> &others);
    void showWiktionary(const QList<metadata::WiktionaryEntry> &entries);
    void clearOnline();

    LibrarySession &m_session;
    qint64 m_publicationId;
    QString m_words;
    QString m_libraryWords; // What the library is being, or was last, searched for.

    QLineEdit *m_edit = nullptr;
    QLabel *m_libraryStatus = nullptr;
    QListWidget *m_libraryList = nullptr;
    QPushButton *m_searchLibrary = nullptr;

    QLabel *m_onlineStatus = nullptr;
    QPushButton *m_wikipediaButton = nullptr;
    QLabel *m_wikipediaStatus = nullptr;
    QLabel *m_wikipediaTitle = nullptr;
    QLabel *m_wikipediaDescription = nullptr;
    QLabel *m_wikipediaExtract = nullptr;
    QListWidget *m_wikipediaOthers = nullptr;
    QPushButton *m_wikipediaOpen = nullptr;
    QLabel *m_wikipediaCredit = nullptr;
    QString m_wikipediaKey;
    QString m_wikipediaAskedLanguage;

    QPushButton *m_wiktionaryButton = nullptr;
    QLabel *m_wiktionaryStatus = nullptr;
    QLabel *m_wiktionaryText = nullptr;
    QPushButton *m_wiktionaryOpen = nullptr;
    QLabel *m_wiktionaryCredit = nullptr;
    QString m_wiktionaryTerm;

    QCheckBox *m_always = nullptr;

    QThread *m_thread = nullptr;
    bool m_again = false;
    std::unique_ptr<metadata::Transport> m_transport;
    std::unique_ptr<metadata::SelectionLookup> m_lookup;
};

} // namespace papertrail::ui
