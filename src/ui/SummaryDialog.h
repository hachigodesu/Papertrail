#pragma once

#include "core/model/Publication.h"
#include "core/repo/FileRepository.h"
#include "summary/SummaryEngine.h"

#include <QDialog>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QThread;

namespace papertrail::ui {

class ApiKeyStore;
class LibrarySession;

// What a summary is based on, said plainly, since it's generated and could be wrong.
QString basisDescription(summary::SummaryBasis basis, bool unsure, bool cutOff);

// The Claude Console, where keys are made and the bill is kept. Wherever it's mentioned, it's a link.
inline constexpr const char *kConsoleKeysUrl = "https://platform.claude.com/settings/keys";
inline constexpr const char *kConsoleBillingUrl = "https://platform.claude.com/settings/billing";
// `text` is plain text with %1 where the Console is named, which becomes a link to `url`.
QLabel *consoleLabel(QWidget *parent, const QString &text, const char *url);

// Everything about getting one summary from Claude. Each step that costs money says what it may cost first, and
// the paper itself only ever goes after a confirmation of its own.
class SummaryDialog : public QDialog {
    Q_OBJECT

public:
    enum class Step { Choosing, Working, Result, ConfirmingPaper, Problem };

    SummaryDialog(LibrarySession &session, ApiKeyStore &keys, metadata::Transport &transport, qint64 publicationId, QWidget *parent = nullptr);
    ~SummaryDialog() override;

    Step step() const;

    // What the buttons do, for tests as well.
    void generate();
    void accept() override; // Keeps the summary.
    void discard();
    void askAboutPaper();
    void sendPaper();
    void backToChoosing();

    QLineEdit *keyEdit() const { return m_keyEdit; }
    QWidget *keyRow() const { return m_keyRow; }
    QComboBox *modelBox() const { return m_model; }
    QComboBox *effortBox() const { return m_effort; }
    QLabel *detailsLabel() const { return m_details; }
    QLabel *estimateLabel() const { return m_estimate; }
    QPushButton *generateButton() const { return m_generate; }
    QLabel *basisLabel() const { return m_basis; }
    QPlainTextEdit *summaryView() const { return m_summaryView; }
    QListWidget *sourceList() const { return m_sources; }
    QLabel *costLabel() const { return m_cost; }
    QPushButton *sendPaperButton() const { return m_sendPaper; }
    QLabel *paperLabel() const { return m_paperDetails; }
    QPushButton *confirmPaperButton() const { return m_confirmPaper; }
    QLabel *problemLabel() const { return m_problem; }

    double spentSoFar() const { return m_spent; }
    bool kept() const { return m_kept; }

signals:
    void stepChanged();
    void paperReady(bool ok, const QString &problem);

private:
    struct PaperPlan {
        bool canSend = false;
        QString cantSendBecause;
        QString description;
        bool isPdf = false;
        int pages = 0;
        qint64 bytes = 0;
        core::FileRecord file;
        std::optional<std::pair<int, int>> pageRange; // Counting from 0, for an article's own pages.
    };

    void buildChoosing();
    void buildWorking();
    void buildResult();
    void buildConfirmingPaper();
    void buildProblem();
    void setStep(Step step);
    void updateEstimate();
    void loadKey();
    void addModelsFromAccount(const QStringList &ids);
    void onFinished(const summary::SummaryOutcome &outcome);
    void onFailed(summary::SummaryFailure failure, const QString &message, const summary::Usage &spent);
    void addToSpent(const QString &model, const summary::Usage &usage);
    QString spentText() const;
    PaperPlan planPaper();
    void startWith(const QString &key);

    LibrarySession &m_session;
    ApiKeyStore &m_keys;
    summary::SummaryEngine *m_engine = nullptr;
    core::Publication m_publication;
    std::optional<summary::SummaryOutcome> m_outcome;
    std::optional<summary::PaperContent> m_paper;
    PaperPlan m_paperPlan;
    QString m_key;
    double m_spent = 0;
    bool m_spentIsExact = true;
    bool m_kept = false;
    QThread *m_paperThread = nullptr;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_keyRow = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QComboBox *m_model = nullptr;
    QComboBox *m_effort = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_estimate = nullptr;
    QPushButton *m_generate = nullptr;
    QLabel *m_working = nullptr;
    QLabel *m_basis = nullptr;
    QPlainTextEdit *m_summaryView = nullptr;
    QListWidget *m_sources = nullptr;
    QLabel *m_cost = nullptr;
    QPushButton *m_sendPaper = nullptr;
    QLabel *m_paperDetails = nullptr;
    QPushButton *m_confirmPaper = nullptr;
    QLabel *m_problem = nullptr;
};

// Enters, replaces or removes the key, from the File menu.
class ApiKeyDialog : public QDialog {
    Q_OBJECT

public:
    explicit ApiKeyDialog(ApiKeyStore &keys, QWidget *parent = nullptr);

private:
    ApiKeyStore &m_keys;
    QLineEdit *m_edit = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace papertrail::ui
