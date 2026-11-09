#include "ui/SummaryDialog.h"

#include "core/repo/PublicationRepository.h"
#include "core/repo/SummaryRepository.h"
#include "core/settings/SettingsStore.h"
#include "import/ImportService.h"
#include "import/WorkerClient.h"
#include "ui/ApiKeyStore.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"
#include "summary/SummarySpending.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>

namespace papertrail::ui {

using namespace core;
using namespace summary;

namespace {

constexpr int kPagesPerRequest = 25;
constexpr int kPaperTimeoutMs = 120000;

QLabel *wrappedLabel(QWidget *parent, const QString &text = QString())
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText); // Text from a file or a source could contain anything.
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

QLabel *consoleLabel(QWidget *parent, const QString &text, const char *url)
{
    auto *label = new QLabel(parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true); // Only ever this fixed address of Anthropic's.
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setText(text.toHtmlEscaped().arg(QStringLiteral("<a href=\"%1\">%2</a>")
                                                .arg(QString::fromLatin1(url), QObject::tr("Claude Console").toHtmlEscaped())));
    return label;
}

QString basisDescription(SummaryBasis basis, bool unsure, bool cutOff, bool isCase)
{
    QString text;
    switch (basis) {
    case SummaryBasis::FullText: text = QObject::tr("Claude read the full text."); break;
    case SummaryBasis::AbstractOnly:
        text = isCase ? QObject::tr("Claude could only read a summary of the judgment, so parts of this may be missing.")
                      : QObject::tr("Claude could only read the abstract, so parts of this may be missing.");
        break;
    case SummaryBasis::OtherSources:
        text = isCase ? QObject::tr("Claude couldn't read the judgment, and used what other sources say about it. Treat this with care.")
                      : QObject::tr("Claude couldn't read the paper, and used what other sources say about it. Treat this with care.");
        break;
    case SummaryBasis::NotFound:
        text = isCase ? QObject::tr("Claude couldn't find this judgment. Treat this with care.")
                      : QObject::tr("Claude couldn't find this paper. Treat this with care.");
        break;
    default: text = QObject::tr("Claude didn't say what this is based on. Treat it with care."); break;
    }
    if (unsure)
        text += isCase ? QObject::tr(" It wasn't sure it had found the right case, so check the sources.")
                       : QObject::tr(" It wasn't sure it had found the right paper, so check the sources.");
    if (cutOff)
        text += QObject::tr(" It reached the longest answer allowed, so the end may be missing.");
    return text;
}

SummaryDialog::SummaryDialog(LibrarySession &session, ApiKeyStore &keys, metadata::Transport &transport, qint64 publicationId, QWidget *parent)
    : QDialog(parent), m_session(session), m_keys(keys)
{
    setWindowTitle(tr("Summarize with Claude"));
    resize(720, 620);

    m_publication = m_session.publications().get(publicationId).value_or(Publication{});
    m_engine = new SummaryEngine(transport, m_session.paths().providerUrls.anthropic, this);
    connect(m_engine, &SummaryEngine::finished, this, &SummaryDialog::onFinished);
    connect(m_engine, &SummaryEngine::failed, this, &SummaryDialog::onFailed);
    connect(m_engine, &SummaryEngine::modelsReady, this, &SummaryDialog::addModelsFromAccount);
    connect(this, &SummaryDialog::paperReady, this, [this](bool ok, const QString &problem) {
        if (m_paperThread) {
            m_paperThread->wait();
            delete m_paperThread;
            m_paperThread = nullptr;
        }
        if (!ok) {
            m_problem->setText(problem);
            setStep(Step::Problem);
            return;
        }
        m_engine->setApiKey(m_key);
        m_engine->generate(m_model->currentData().toString(), m_effort->currentData().toString(), m_publication, m_paper);
        m_paper.reset(); // It may be large, and the engine has its own copy now.
    });

    auto *layout = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack);
    buildChoosing();
    buildWorking();
    buildResult();
    buildConfirmingPaper();
    buildProblem();
    setStep(Step::Choosing);
    updateEstimate();
    loadKey();
}

SummaryDialog::~SummaryDialog()
{
    if (m_paperThread) {
        m_paperThread->wait();
        delete m_paperThread;
    }
}

SummaryDialog::Step SummaryDialog::step() const
{
    return Step(m_stack->currentIndex());
}

void SummaryDialog::setStep(Step step)
{
    m_stack->setCurrentIndex(int(step));
    emit stepChanged();
}

void SummaryDialog::buildChoosing()
{
    auto *page = new QWidget(m_stack);
    auto *column = new QVBoxLayout(page);

    column->addWidget(wrappedLabel(page, tr("Claude will look for this publication online and summarize what it can read. "
                                            "Only these details are sent to Anthropic, and not the file:")));
    QStringList lines;
    for (const IdentifyingDetail &detail : identifyingDetails(m_publication))
        lines << detail.label + QStringLiteral(": ") + detail.value;
    m_details = wrappedLabel(page, lines.join(u'\n'));
    column->addWidget(m_details);

    m_keyRow = new QWidget(page);
    auto *keyLayout = new QFormLayout(m_keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    m_keyEdit = new QLineEdit(m_keyRow);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(tr("Your API key, from the Claude Console"));
    m_keyEdit->setAccessibleName(tr("Claude API key"));
    connect(m_keyEdit, &QLineEdit::textChanged, this, [this] { m_generate->setEnabled(!m_keyEdit->text().trimmed().isEmpty() || !m_key.isEmpty()); });
    keyLayout->addRow(tr("API key"), m_keyEdit);
    keyLayout->addRow(QString(), consoleLabel(m_keyRow, tr("Make a key in the %1, and keep it somewhere safe."), kConsoleKeysUrl));
    column->addWidget(m_keyRow);

    auto *form = new QFormLayout;
    m_model = new QComboBox(page);
    for (const ModelInfo &model : knownModels())
        m_model->addItem(model.label, model.id);
    const int savedModel = m_model->findData(m_session.settings().value(QStringLiteral("summary.model")).toString());
    m_model->setCurrentIndex(savedModel >= 0 ? savedModel : m_model->findData(defaultModel()));
    form->addRow(tr("Model"), m_model);

    m_effort = new QComboBox(page);
    for (const QString &effort : effortLevels())
        m_effort->addItem(effortLabel(effort), effort);
    const int savedEffort = m_effort->findData(m_session.settings().value(QStringLiteral("summary.effort")).toString());
    m_effort->setCurrentIndex(savedEffort >= 0 ? savedEffort : m_effort->findData(defaultEffort()));
    form->addRow(tr("Effort"), m_effort);
    column->addLayout(form);

    m_estimate = wrappedLabel(page);
    QFont bold = m_estimate->font();
    bold.setBold(true);
    m_estimate->setFont(bold);
    column->addWidget(m_estimate);
    column->addWidget(wrappedLabel(page, tr("You're charged by Anthropic for this whether or not you keep the summary. Prices were checked on %1, "
                                            "so check them against your account.").arg(pricesCheckedOn())));
    column->addStretch();

    connect(m_model, &QComboBox::currentIndexChanged, this, &SummaryDialog::updateEstimate);
    connect(m_effort, &QComboBox::currentIndexChanged, this, &SummaryDialog::updateEstimate);

    auto *buttons = new QDialogButtonBox(page);
    m_generate = buttons->addButton(tr("Generate summary"), QDialogButtonBox::ActionRole);
    m_generate->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(m_generate, &QPushButton::clicked, this, &SummaryDialog::generate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);
    m_stack->addWidget(page);
}

void SummaryDialog::buildWorking()
{
    auto *page = new QWidget(m_stack);
    auto *column = new QVBoxLayout(page);
    column->addStretch();
    m_working = wrappedLabel(page, tr("Claude is working on it. This can take a few minutes."));
    column->addWidget(m_working);
    auto *bar = new QProgressBar(page);
    bar->setRange(0, 0);
    column->addWidget(bar);
    column->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, page);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        m_engine->cancel();
        m_problem->setText(tr("Stopped. Anything Anthropic had already started on may still be charged for."));
        setStep(Step::Problem);
    });
    column->addWidget(buttons);
    m_stack->addWidget(page);
}

void SummaryDialog::buildResult()
{
    auto *page = new QWidget(m_stack);
    auto *column = new QVBoxLayout(page);
    m_basis = wrappedLabel(page);
    QFont bold = m_basis->font();
    bold.setBold(true);
    m_basis->setFont(bold);
    column->addWidget(m_basis);

    m_summaryView = new QPlainTextEdit(page); // As it is, so that nothing in a reply can become formatting.
    m_summaryView->setReadOnly(true);
    m_summaryView->setAccessibleName(tr("The summary"));
    column->addWidget(m_summaryView, 1);

    column->addWidget(wrappedLabel(page, tr("Sources it read, which you can double-click to check:")));
    m_sources = new QListWidget(page);
    m_sources->setMaximumHeight(90);
    connect(m_sources, &QListWidget::itemActivated, this, [](QListWidgetItem *item) {
        // These come from a web page or a model, so they can't be trusted.
        const QUrl url = safeWebLink(item->data(Qt::UserRole).toString());
        if (url.isValid())
            QDesktopServices::openUrl(url);
    });
    column->addWidget(m_sources);

    m_cost = wrappedLabel(page);
    column->addWidget(m_cost);

    auto *buttons = new QHBoxLayout;
    m_sendPaper = new QPushButton(tr("Send the paper to Claude..."), page);
    m_sendPaper->setToolTip(tr("A last resort, if this isn't good enough. It sends your file to Anthropic, and costs more."));
    connect(m_sendPaper, &QPushButton::clicked, this, &SummaryDialog::askAboutPaper);
    buttons->addWidget(m_sendPaper);
    buttons->addStretch();
    auto *discardButton = new QPushButton(tr("Discard"), page);
    connect(discardButton, &QPushButton::clicked, this, &SummaryDialog::discard);
    buttons->addWidget(discardButton);
    auto *acceptButton = new QPushButton(tr("Accept"), page);
    connect(acceptButton, &QPushButton::clicked, this, &SummaryDialog::accept);
    buttons->addWidget(acceptButton);
    column->addLayout(buttons);
    m_stack->addWidget(page);
}

void SummaryDialog::buildConfirmingPaper()
{
    auto *page = new QWidget(m_stack);
    auto *column = new QVBoxLayout(page);
    m_paperDetails = wrappedLabel(page);
    column->addWidget(m_paperDetails);
    column->addStretch();
    auto *buttons = new QHBoxLayout;
    auto *back = new QPushButton(tr("Back"), page);
    connect(back, &QPushButton::clicked, this, [this] { setStep(m_outcome ? Step::Result : Step::Problem); });
    buttons->addWidget(back);
    buttons->addStretch();
    m_confirmPaper = new QPushButton(tr("Send the paper"), page);
    connect(m_confirmPaper, &QPushButton::clicked, this, &SummaryDialog::sendPaper);
    buttons->addWidget(m_confirmPaper);
    column->addLayout(buttons);
    m_stack->addWidget(page);
}

void SummaryDialog::buildProblem()
{
    auto *page = new QWidget(m_stack);
    auto *column = new QVBoxLayout(page);
    m_problem = wrappedLabel(page);
    column->addWidget(m_problem);
    column->addStretch();
    auto *buttons = new QHBoxLayout;
    auto *paper = new QPushButton(tr("Send the paper to Claude..."), page);
    connect(paper, &QPushButton::clicked, this, &SummaryDialog::askAboutPaper);
    buttons->addWidget(paper);
    buttons->addStretch();
    auto *again = new QPushButton(tr("Try again..."), page);
    again->setToolTip(tr("Back to choosing a model and effort, with a new estimate."));
    connect(again, &QPushButton::clicked, this, &SummaryDialog::backToChoosing);
    buttons->addWidget(again);
    auto *close = new QPushButton(tr("Close"), page);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(close);
    column->addLayout(buttons);
    m_stack->addWidget(page);
}

void SummaryDialog::loadKey()
{
    m_keys.read([this](const QString &key) {
        m_key = key;
        m_keyRow->setVisible(key.isEmpty());
        m_generate->setEnabled(!key.isEmpty() || !m_keyEdit->text().trimmed().isEmpty());
        if (!key.isEmpty()) {
            m_engine->setApiKey(key);
            m_engine->fetchModels();
        }
    });
}

void SummaryDialog::addModelsFromAccount(const QStringList &ids)
{
    for (const QString &id : ids) {
        if (m_model->findData(id) < 0)
            m_model->addItem(tr("%1 (no price known)").arg(id), id);
    }
}

void SummaryDialog::updateEstimate()
{
    const std::optional<CostRange> range = estimateFromDetails(m_model->currentData().toString(), m_effort->currentData().toString(), m_publication);
    m_estimate->setText(range ? tr("Estimated cost: %1").arg(formatRange(*range))
                              : tr("Papertrail has no price for this model, so it can't estimate the cost."));
}

void SummaryDialog::backToChoosing()
{
    m_outcome.reset();
    setStep(Step::Choosing);
}

void SummaryDialog::startWith(const QString &key)
{
    m_key = key;
    m_engine->setApiKey(key);
    m_session.settings().setValue(QStringLiteral("summary.model"), m_model->currentData().toString());
    m_session.settings().setValue(QStringLiteral("summary.effort"), m_effort->currentData().toString());
    m_working->setText(tr("Claude is working on it. This can take a few minutes."));
    setStep(Step::Working);
    m_engine->generate(m_model->currentData().toString(), m_effort->currentData().toString(), m_publication);
}

void SummaryDialog::generate()
{
    if (!m_key.isEmpty()) {
        startWith(m_key);
        return;
    }
    const QString key = m_keyEdit->text().trimmed();
    if (key.isEmpty())
        return;
    m_keyEdit->clear();
    m_keys.write(key, [this, key](bool) {
        m_keyRow->hide();
        startWith(key);
    });
}

void SummaryDialog::addToSpent(const QString &model, const Usage &usage)
{
    // Noted in the library too, since it's charged for whether or not the summary is kept.
    try {
        recordSpend(m_session.database(), QStringLiteral("instant:") + QUuid::createUuid().toString(QUuid::WithoutBraces), m_publication.id,
                    model, SpendKind::Instant, usage);
    } catch (const std::exception &) {
        // Only the record of spending is lost, and the dialog still says what this cost.
    }
    if (const std::optional<double> cost = costOf(model, usage))
        m_spent += *cost;
    else if (usage.inputTokens > 0 || usage.outputTokens > 0)
        m_spentIsExact = false;
}

QString SummaryDialog::spentText() const
{
    return m_spentIsExact ? formatDollars(m_spent) : tr("at least %1").arg(formatDollars(m_spent));
}

void SummaryDialog::onFinished(const SummaryOutcome &outcome)
{
    m_outcome = outcome;
    addToSpent(outcome.model, outcome.usage);
    m_basis->setText(basisDescription(outcome.summary.basis, outcome.summary.identificationUnsure, outcome.cutOff,
                                      outcome.summary.isCase));
    m_summaryView->setPlainText(summaryAsText(outcome.summary));

    m_sources->clear();
    for (const Source &source : outcome.sources) {
        auto *item = new QListWidgetItem(source.title.isEmpty() ? source.url : source.title + QStringLiteral(" (") + source.url + u')', m_sources);
        item->setData(Qt::UserRole, source.url);
    }
    if (outcome.sentPaper && m_sources->count() == 0)
        new QListWidgetItem(tr("The paper you sent."), m_sources);

    const QString thisOne = outcome.cost ? formatDollars(*outcome.cost) : tr("unknown, as this model's price isn't known");
    m_cost->setText(tr("This cost %1 (%2 tokens in, %3 out, %4 web searches). Spent so far on this: %5.")
                        .arg(thisOne)
                        .arg(outcome.usage.inputTokens)
                        .arg(outcome.usage.outputTokens)
                        .arg(outcome.usage.webSearches)
                        .arg(spentText()));
    m_sendPaper->setVisible(!outcome.sentPaper);
    setStep(Step::Result);
}

void SummaryDialog::onFailed(SummaryFailure failure, const QString &message, const Usage &spent)
{
    addToSpent(m_model->currentData().toString(), spent);
    QString text = message;
    if (failure == SummaryFailure::Refused)
        text += tr("\n\nThis can happen with some subjects, such as biomedical papers. You can try another model, with a new estimate.");
    if (failure == SummaryFailure::BadKey) {
        m_keys.clear();
        m_key.clear();
        m_keyRow->show();
    }
    if (m_spent > 0)
        text += tr("\n\nSpent so far on this: %1.").arg(spentText());
    m_problem->setText(text);
    setStep(Step::Problem);
}

SummaryDialog::PaperPlan SummaryDialog::planPaper()
{
    PaperPlan plan;
    const qint64 owner = m_publication.parentId.value_or(m_publication.id);
    const QList<FileRecord> files = m_session.files().filesFor(owner);
    for (const FileRecord &file : files) {
        if (!QFileInfo::exists(m_session.files().absolutePath(file.id)))
            continue;
        // A PDF's own layout and figures are worth having if it's there.
        if (plan.file.id == 0 || file.format == QLatin1StringView("pdf"))
            plan.file = file;
    }
    if (plan.file.id == 0) {
        plan.cantSendBecause = tr("This publication's file can't be found right now.");
        return plan;
    }

    const QString path = m_session.files().absolutePath(plan.file.id);
    plan.bytes = QFileInfo(path).size();
    plan.pages = plan.file.pageCount.value_or(0);
    plan.isPdf = plan.file.format == QLatin1StringView("pdf");
    const QString name = QFileInfo(path).fileName();

    if (m_publication.parentId) {
        // Only an article's own pages, and not the whole volume, which means sending their text.
        if (!m_publication.pageStart || !m_publication.pageEnd) {
            plan.cantSendBecause = tr("This article has no page range, so there's no telling which part of the file to send.");
            return plan;
        }
        if (plan.file.hasTextLayer && !*plan.file.hasTextLayer) {
            plan.cantSendBecause = tr("The file this article is in has no text to send, and sending all of it would cost far more than this one article.");
            return plan;
        }
        plan.isPdf = false;
        plan.pageRange = std::make_pair(*m_publication.pageStart - 1, *m_publication.pageEnd - 1);
        plan.pages = *m_publication.pageEnd - *m_publication.pageStart + 1;
        plan.description = tr("The text of pages %1 to %2 of %3").arg(*m_publication.pageStart).arg(*m_publication.pageEnd).arg(name);
        plan.canSend = true;
        return plan;
    }

    if (plan.isPdf) {
        const qint64 encoded = (plan.bytes + 2) / 3 * 4;
        if (encoded > kLargestRequestBytes - 1024 * 1024 || plan.pages > kMostPdfPages) {
            plan.cantSendBecause = tr("This PDF is larger than Anthropic allows, at %1 MB and %2 pages. The limits are about 23 MB and %3 pages.")
                                       .arg(plan.bytes / (1024 * 1024))
                                       .arg(plan.pages)
                                       .arg(kMostPdfPages);
            return plan;
        }
        plan.description = tr("%1, a PDF of %2 pages and %3 MB").arg(name).arg(plan.pages).arg(QString::number(double(plan.bytes) / (1024 * 1024), 'f', 1));
    } else {
        if (plan.file.hasTextLayer && !*plan.file.hasTextLayer) {
            plan.cantSendBecause = tr("This %1 file has no text to send. Only PDFs can be sent as they are.").arg(plan.file.format.toUpper());
            return plan;
        }
        plan.description = tr("The text of %1, of %2 pages").arg(name).arg(plan.pages);
    }
    plan.canSend = true;
    return plan;
}

void SummaryDialog::askAboutPaper()
{
    m_paperPlan = planPaper();
    if (!m_paperPlan.canSend) {
        m_paperDetails->setText(m_paperPlan.cantSendBecause);
        m_confirmPaper->setEnabled(false);
        setStep(Step::ConfirmingPaper);
        return;
    }

    const std::optional<CostRange> range = estimateFromPaper(m_model->currentData().toString(), m_effort->currentData().toString(), m_paperPlan.pages, m_paperPlan.isPdf);
    const QString estimate = range ? tr("Estimated cost of this: %1.").arg(formatRange(*range)) : tr("Papertrail has no price for this model, so it can't estimate the cost.");
    m_paperDetails->setText(
        tr("This sends the paper itself to Anthropic, using %1 at %2 effort:\n\n%3\n\n"
           "It leaves this computer, and it's up to you whether you're allowed to share it. Your file isn't changed.\n\n"
           "%4\nSpent so far on this: %5.\n\nThis costs money whether or not you keep the summary.")
            .arg(m_model->currentText(), m_effort->currentText(), m_paperPlan.description, estimate, spentText()));
    m_confirmPaper->setEnabled(true);
    setStep(Step::ConfirmingPaper);
}

void SummaryDialog::sendPaper()
{
    if (!m_paperPlan.canSend || m_paperThread)
        return;
    m_working->setText(tr("Sending the paper to Claude. This can take a few minutes."));
    setStep(Step::Working);
    m_paper.reset();

    const QString path = m_session.files().absolutePath(m_paperPlan.file.id);
    const PaperPlan plan = m_paperPlan;
    const QString worker = m_session.paths().workerPath;
    const QString converted = plan.file.sha256.isEmpty() ? QString() : import::convertedPathFor(m_session.paths().cacheDirectory, plan.file.sha256);

    // Reading a large file, or having the worker read its text, mustn't hold up the window.
    m_paperThread = QThread::create([this, path, plan, worker, converted] {
        PaperContent paper;
        if (plan.isPdf) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                emit paperReady(false, tr("The file couldn't be read."));
                return;
            }
            paper.pdfBase64 = file.readAll().toBase64();
        } else {
            import::WorkerClient client(worker);
            const int first = plan.pageRange ? plan.pageRange->first : 0;
            int page = first;
            while (true) {
                // With no page range, this goes on until the worker says it has reached the end.
                const int count = plan.pageRange ? qMin(kPagesPerRequest, plan.pageRange->second - page + 1) : kPagesPerRequest;
                if (count <= 0)
                    break;
                const import::WorkerClient::PageTexts texts = client.pageTexts(path, converted, page, count, kPaperTimeoutMs);
                if (texts.failure != import::WorkerFailure::None) {
                    emit paperReady(false, tr("The file's text couldn't be read: %1").arg(texts.error));
                    return;
                }
                paper.text += texts.pages.join(u'\n') + u'\n';
                page += int(texts.pages.size());
                if (texts.pages.isEmpty() || page >= texts.pageCount)
                    break;
                // Nothing is ever cut short without saying so.
                if (paper.text.size() > kMostTextCharacters) {
                    emit paperReady(false, tr("This file has too much text to send. Sending only part of it could give a misleading summary."));
                    return;
                }
            }
            if (paper.text.trimmed().isEmpty()) {
                emit paperReady(false, tr("This file has no text to send."));
                return;
            }
        }
        m_paper = paper;
        emit paperReady(true, QString());
    });
    m_paperThread->start();
}

void SummaryDialog::accept()
{
    if (!m_outcome)
        return;
    StoredSummary stored;
    stored.publicationId = m_publication.id;
    stored.model = m_outcome->model;
    stored.effort = m_outcome->effort;
    stored.basis = basisToText(m_outcome->summary.basis);
    stored.identificationUnsure = m_outcome->summary.identificationUnsure;
    stored.text = summaryAsText(m_outcome->summary);
    for (const Source &source : std::as_const(m_outcome->sources))
        stored.sources << qMakePair(source.url, source.title);
    stored.costDollars = m_outcome->cost;
    stored.sentPaper = m_outcome->sentPaper;
    try {
        SummaryRepository(m_session.database()).save(stored);
    } catch (const std::exception &error) {
        m_problem->setText(tr("The summary couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        setStep(Step::Problem);
        return;
    }
    m_kept = true;
    QDialog::accept();
}

void SummaryDialog::discard()
{
    m_outcome.reset();
    reject();
}

ApiKeyDialog::ApiKeyDialog(ApiKeyStore &keys, QWidget *parent)
    : QDialog(parent), m_keys(keys)
{
    setWindowTitle(tr("Claude API key"));
    auto *column = new QVBoxLayout(this);
    column->addWidget(consoleLabel(this, tr("Summaries use your own API key, from the %1, and you're charged for them by Anthropic. "
                                            "It's kept in this computer's credential store, and never in Papertrail's database, settings or exports."),
                                   kConsoleKeysUrl));
    m_edit = new QLineEdit(this);
    m_edit->setEchoMode(QLineEdit::Password);
    m_edit->setAccessibleName(tr("Claude API key"));
    column->addWidget(m_edit);
    m_status = wrappedLabel(this);
    column->addWidget(m_status);

    keys.read([this](const QString &key) { m_status->setText(key.isEmpty() ? tr("No key is saved.") : tr("A key is saved.")); });

    auto *buttons = new QDialogButtonBox(this);
    auto *save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    auto *remove = buttons->addButton(tr("Remove the key"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(save, &QPushButton::clicked, this, [this] {
        const QString key = m_edit->text().trimmed();
        if (key.isEmpty())
            return;
        m_edit->clear();
        m_keys.write(key, [this](bool permanently) {
            m_status->setText(permanently ? tr("The key has been saved.")
                                          : tr("This computer has no credential store available, so the key will only last until Papertrail closes."));
        });
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        m_keys.clear();
        m_status->setText(tr("The key has been removed."));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);
}

} // namespace papertrail::ui
