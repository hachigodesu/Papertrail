#include "ui/OnlineLookupDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

namespace papertrail::ui {

OnlineLookupDialog::OnlineLookupDialog(const QString &heading, const QString &mode, const QString &email, bool showEmail,
                                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Online lookups"));
    resize(560, showEmail ? 420 : 360);
    auto *column = new QVBoxLayout(this);

    const auto addText = [&](const QString &text, bool quiet = false) {
        auto *label = new QLabel(text, this);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        if (quiet) {
            QPalette palette = label->palette();
            palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
            label->setPalette(palette);
            label->setContentsMargins(22, 0, 0, 8);
        }
        column->addWidget(label);
    };

    if (!heading.isEmpty()) {
        addText(heading);
        column->addSpacing(6);
    }
    addText(tr("Papertrail can fill in missing details, such as authors and journal names, from online sources."));
    column->addSpacing(6);

    m_review = new QRadioButton(tr("Look up details, and let me check the uncertain ones"), this);
    column->addWidget(m_review);
    addText(tr("Matches on an exact DOI, ISBN or arXiv number apply by themselves, if the title agrees. "
               "Anything else waits for you, with both versions side by side."), true);

    m_auto = new QRadioButton(tr("Look up details, and apply the best match"), this);
    column->addWidget(m_auto);
    addText(tr("Nothing waits for you. Matches that aren't certain are marked as needing a check, and any match can be undone."), true);

    m_offline = new QRadioButton(tr("Stay offline"), this);
    column->addWidget(m_offline);
    addText(tr("Only what's inside your files is used, and nothing is ever sent anywhere."), true);

    addText(tr("Lookups send a DOI, ISBN, arXiv number or title to Crossref, OpenAlex, arXiv or Open Library. "
               "Your files, notes and tags are never sent."));

    if (showEmail) {
        column->addSpacing(6);
        auto *form = new QFormLayout;
        m_email = new QLineEdit(email, this);
        m_email->setPlaceholderText(tr("Optional"));
        m_email->setAccessibleName(tr("Contact email for Crossref and OpenAlex"));
        form->addRow(tr("Contact email"), m_email);
        column->addLayout(form);
        addText(tr("Crossref and OpenAlex ask for an address to contact if something goes wrong, and may respond faster. "
                   "It's sent with requests to those two only, and only if you enter one."), true);

        column->addSpacing(6);
        m_notices = new QCheckBox(tr("Check now and then for retractions and concerns raised"), this);
        m_notices->setChecked(true);
        column->addWidget(m_notices);
        addText(tr("Sends the DOIs in your library to Crossref, about once a month each, to find retractions, "
                   "corrections and expressions of concern published since. Only when lookups are online."), true);
    }

    column->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);

    setMode(mode);
}

QString OnlineLookupDialog::mode() const
{
    if (m_review->isChecked())
        return QStringLiteral("online_review");
    if (m_auto->isChecked())
        return QStringLiteral("online_auto");
    return QStringLiteral("offline");
}

void OnlineLookupDialog::setMode(const QString &mode)
{
    if (mode == QLatin1StringView("online_review"))
        m_review->setChecked(true);
    else if (mode == QLatin1StringView("online_auto"))
        m_auto->setChecked(true);
    else
        m_offline->setChecked(true);
}

void OnlineLookupDialog::setCheckNotices(bool check)
{
    if (m_notices)
        m_notices->setChecked(check);
}

bool OnlineLookupDialog::checkNotices() const
{
    return m_notices && m_notices->isChecked();
}

QString OnlineLookupDialog::email() const
{
    return m_email ? m_email->text().trimmed() : QString();
}

} // namespace papertrail::ui
