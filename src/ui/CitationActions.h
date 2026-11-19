#pragma once

#include "citation/CslStyles.h"
#include "citation/Exporters.h"
#include "citation/Styles.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QMenu;
class QProgressDialog;
class QThread;
class QWidget;

namespace papertrail::citation {
class CslThread;
}

namespace papertrail::ui {

class CitationStylesDialog;
class LibrarySession;

// Copying references and in-text citations, and exporting citations to a file, for any publications.
class CitationActions : public QObject {
    Q_OBJECT

public:
    // More than this are exported to a file rather than copied, since a reference list that long is
    // no use pasted.
    static constexpr int kMaxToCopy = 500;
    // How many CSL styles chosen lately the Style menu offers.
    static constexpr int kRecentStyles = 5;

    CitationActions(LibrarySession &session, QWidget *window);
    ~CitationActions() override;

    // The style chosen, remembered in the citation.style setting: one of the hand-written styles' keys, or a CSL
    // style's (citation::cslStyleKey).
    QString styleKey() const;
    void setStyleKey(const QString &key);
    // The hand-written style chosen, or Harvard while a CSL style is.
    citation::CitationStyle style() const;
    void setStyle(citation::CitationStyle style);
    // A style's name as shown, such as "APA (7th edition)" or a CSL style's title.
    QString styleTitle(const QString &key) const;
    // The CSL styles chosen lately, newest first, as keys.
    QStringList recentStyleKeys() const;
    // A CSL style ready to render, from those bundled or added to this library, in the locale chosen.
    std::optional<citation::CslLayout> cslLayout(const QString &key, QString *error = nullptr) const;
    // Made when first needed, and shared with the styles dialog.
    citation::CslThread *cslThread();

    // Adds a "Cite" menu for `count` publications. Which ones is only asked for when it's used, since working
    // out a large selection takes a moment.
    using IdSource = std::function<QList<qint64>()>;
    // For case law only, it offers each legal form instead, since cases are never cited in the chosen style's.
    QMenu *addCiteMenu(QMenu *menu, qint64 count, const IdSource &ids, bool onlyCases = false);
    // Fills a menu with the same actions.
    void fillCiteMenu(QMenu *menu, qint64 count, const IdSource &ids, bool onlyCases = false);

    // Puts formatted text on the clipboard, with italics for word processors and as plain text. These return
    // whether they could, or for a CSL style whether they've started: `copyFinished` follows.
    bool copyReferences(const QList<qint64> &ids);
    bool copyInTextCitation(const QList<qint64> &ids);
    // Cases in the Bluebook's form, or OSCOLA's, whichever each is set to be cited in.
    bool copyCaseCitations(const QList<qint64> &ids, bool oscola);
    bool isCopying() const { return m_copyRequest != 0; }

    // The styles there are, with a preview of these publications, and a way to add one from a file. One at a time.
    // `parent` is the window it belongs to, such as the Settings window, which would otherwise block it.
    CitationStylesDialog *showStylesDialog(const QList<qint64> &ids = {}, QWidget *parent = nullptr);

    // Asks for a file, then exports in the background.
    void askAndExport(const QList<qint64> &ids, const QString &suggestedName = QString());
    // Exports to this file in the background. `exportFinished` follows.
    void startExport(const QList<qint64> &ids, const QString &path, citation::ExportFormat format);
    bool isExporting() const { return m_thread != nullptr; }

signals:
    void message(const QString &text);
    void exportFinished(bool ok, const QString &message);
    // A copy in a CSL style is on the clipboard, or couldn't be made.
    void copyFinished(bool ok);
    void styleChanged(const QString &key);

private:
    QList<citation::CitationItem> itemsFor(const QList<qint64> &ids);
    citation::StyleOptions styleOptions() const;
    // The CSL style to copy in, or nothing for a hand-written one. One that can't be used is said so, and Harvard
    // is used instead.
    std::optional<citation::CslLayout> cslToCopyIn();
    void startCslCopy(const citation::CslLayout &layout, const QList<citation::CitationItem> &items, bool references);
    void onCslFinished(qint64 request, const QList<citation::RichText> &entries, const QList<int> &order, const QString &error);

    LibrarySession &m_session;
    QWidget *m_window;
    QThread *m_thread = nullptr;
    QProgressDialog *m_progress = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    citation::CslThread *m_csl = nullptr;
    qint64 m_copyRequest = 0;
    QString m_copyText; // What to say once the copy is made.
    QPointer<CitationStylesDialog> m_stylesDialog;
};

} // namespace papertrail::ui
