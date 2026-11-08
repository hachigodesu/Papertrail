#include "ui/BatchSummaryDialog.h"

#include "core/repo/SummaryRepository.h"
#include "summary/SummaryBatches.h"
#include "summary/SummaryRequest.h"
#include "ui/LibrarySession.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace papertrail::ui {

using namespace summary;

BatchSummaryDialog::BatchSummaryDialog(LibrarySession &session, const QList<qint64> &publicationIds, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
{
    setWindowTitle(tr("Summarize later at half price"));
    resize(560, 380);

    core::SummaryRepository summaries(session.database());
    for (const qint64 id : publicationIds) {
        const std::optional<core::Publication> publication = session.publications().get(id);
        if (!publication)
            continue;
        // One already on its way, or back and waiting to be reviewed, isn't asked for again.
        const std::optional<BatchEntry> pending = SummaryBatches::entryFor(session.database(), id);
        if (pending && pending->state != BatchEntry::State::Failed)
            ++m_pending;
        else if (summaries.forPublication(id))
            m_with << *publication;
        else
            m_without << *publication;
    }

    auto *layout = new QVBoxLayout(this);
    const auto addText = [&](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        layout->addWidget(label);
        return label;
    };
    addText(tr("These are sent to Anthropic together, as a batch. It answers within a day, usually within the hour, for half "
               "of what asking straight away costs. Papertrail collects each summary while it's open, and keeps it for you to "
               "review in View > Summaries from Claude, so closing Papertrail loses nothing."));
    addText(tr("Only what identifies each publication is sent: its title, authors, year, journal or publisher, and "
               "identifiers. Never the paper itself, your notes or your tags. Claude finds the paper on the web if it can."));

    auto *form = new QFormLayout;
    m_model = new QComboBox(this);
    for (const ModelInfo &info : knownModels())
        m_model->addItem(info.label, info.id);
    m_effort = new QComboBox(this);
    for (const QString &level : effortLevels())
        m_effort->addItem(effortLabel(level), level);
    const int savedModel = m_model->findData(session.settings().value(QStringLiteral("summary.model")).toString());
    m_model->setCurrentIndex(savedModel >= 0 ? savedModel : qMax(0, m_model->findData(defaultModel())));
    const int savedEffort = m_effort->findData(session.settings().value(QStringLiteral("summary.effort")).toString());
    m_effort->setCurrentIndex(savedEffort >= 0 ? savedEffort : qMax(0, m_effort->findData(defaultEffort())));
    form->addRow(tr("Model"), m_model);
    form->addRow(tr("Effort"), m_effort);
    layout->addLayout(form);

    m_replace = new QCheckBox(this);
    m_replace->setText(m_with.size() == 1 ? tr("Also send the 1 that has a summary already. Keeping the new one replaces it.")
                                          : tr("Also send the %1 that have summaries already. Keeping a new one replaces the old.").arg(m_with.size()));
    m_replace->setVisible(!m_with.isEmpty());
    layout->addWidget(m_replace);

    m_estimate = new QLabel(this);
    m_estimate->setWordWrap(true);
    m_estimate->setTextFormat(Qt::PlainText);
    layout->addWidget(m_estimate);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_send = buttons->addButton(tr("Send"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_model, &QComboBox::currentIndexChanged, this, &BatchSummaryDialog::update);
    connect(m_effort, &QComboBox::currentIndexChanged, this, &BatchSummaryDialog::update);
    connect(m_replace, &QCheckBox::toggled, this, &BatchSummaryDialog::update);
    connect(this, &QDialog::accepted, this, [this] {
        m_session.settings().setValue(QStringLiteral("summary.model"), model());
        m_session.settings().setValue(QStringLiteral("summary.effort"), effort());
    });
    update();
}

QList<core::Publication> BatchSummaryDialog::included() const
{
    QList<core::Publication> chosen = m_without;
    if (m_replace->isChecked())
        chosen += m_with;
    return chosen.mid(0, kMostAtOnce);
}

QString BatchSummaryDialog::model() const
{
    return m_model->currentData().toString();
}

QString BatchSummaryDialog::effort() const
{
    return m_effort->currentData().toString();
}

void BatchSummaryDialog::update()
{
    const QList<core::Publication> chosen = included();
    double low = 0;
    double high = 0;
    bool known = true;
    for (const core::Publication &publication : chosen) {
        const std::optional<CostRange> range = estimateFromDetails(model(), effort(), publication);
        if (!range) {
            known = false;
            break;
        }
        low += range->low * kBatchPriceFactor;
        high += range->high * kBatchPriceFactor;
    }
    QStringList lines;
    const int total = int(m_without.size() + (m_replace->isChecked() ? m_with.size() : 0));
    lines << (chosen.size() == 1 ? tr("1 publication will be sent.") : tr("%1 publications will be sent.").arg(chosen.size()));
    if (total > chosen.size())
        lines << tr("Only the first %1 go in one batch. Send the rest afterwards.").arg(kMostAtOnce);
    if (!m_with.isEmpty() && !m_replace->isChecked())
        lines << (m_with.size() == 1 ? tr("1 is left out, since it has a summary already.")
                                     : tr("%1 are left out, since they have summaries already.").arg(m_with.size()));
    if (m_pending > 0)
        lines << (m_pending == 1 ? tr("1 is left out, since its summary is already on its way or waiting to be reviewed.")
                                 : tr("%1 are left out, since their summaries are already on their way or waiting to be reviewed.").arg(m_pending));
    if (!chosen.isEmpty()) {
        lines << (known ? tr("It may cost %1 in all, at half price. That's a range, since how long each answer is and how much "
                             "the web tools read can't be known beforehand.")
                              .arg(formatRange(CostRange{low, high}))
                        : tr("This model's prices aren't known, so there's no estimate."));
    }
    m_estimate->setText(lines.join(u'\n'));
    m_send->setEnabled(!chosen.isEmpty());
    m_send->setText(chosen.size() == 1 ? tr("Send 1") : tr("Send %1").arg(chosen.size()));
}

} // namespace papertrail::ui
