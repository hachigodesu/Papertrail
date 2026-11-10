#pragma once

#include "citation/Exporters.h"
#include "citation/Styles.h"

#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QMenu;
class QProgressDialog;
class QThread;
class QWidget;

namespace papertrail::ui {

class LibrarySession;

// Copying references and in-text citations, and exporting citations to a file, for any publications.
class CitationActions : public QObject {
    Q_OBJECT

public:
    // More than this are exported to a file rather than copied, since a reference list that long is
    // no use pasted.
    static constexpr int kMaxToCopy = 500;

    CitationActions(LibrarySession &session, QWidget *window);
    ~CitationActions() override;

    // The style chosen, remembered in the citation.style setting.
    citation::CitationStyle style() const;
    void setStyle(citation::CitationStyle style);

    // Adds a "Cite" menu for `count` publications. Which ones is only asked for when it's used, since working
    // out a large selection takes a moment.
    using IdSource = std::function<QList<qint64>()>;
    // For case law only, it offers each legal form instead, since cases are never cited in the chosen style's.
    QMenu *addCiteMenu(QMenu *menu, qint64 count, const IdSource &ids, bool onlyCases = false);
    // Fills a menu with the same actions.
    void fillCiteMenu(QMenu *menu, qint64 count, const IdSource &ids, bool onlyCases = false);

    // Puts formatted text on the clipboard, with italics for word processors and as plain text. These return
    // whether they could.
    bool copyReferences(const QList<qint64> &ids);
    bool copyInTextCitation(const QList<qint64> &ids);
    // Cases in the Bluebook's form, or OSCOLA's, whichever each is set to be cited in.
    bool copyCaseCitations(const QList<qint64> &ids, bool oscola);

    // Asks for a file, then exports in the background.
    void askAndExport(const QList<qint64> &ids);
    // Exports to this file in the background. `exportFinished` follows.
    void startExport(const QList<qint64> &ids, const QString &path, citation::ExportFormat format);
    bool isExporting() const { return m_thread != nullptr; }

signals:
    void message(const QString &text);
    void exportFinished(bool ok, const QString &message);

private:
    QList<citation::CitationItem> itemsFor(const QList<qint64> &ids);
    citation::StyleOptions styleOptions() const;

    LibrarySession &m_session;
    QWidget *m_window;
    QThread *m_thread = nullptr;
    QProgressDialog *m_progress = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

} // namespace papertrail::ui
