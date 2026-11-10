#include "ui/KeyField.h"

#include "ui/ApiKeyStore.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace papertrail::ui {

KeyField::KeyField(ApiKeyStore &keys, const QString &accessibleName, const std::function<void(const QString &)> &changed,
                   QWidget *parent)
    : QWidget(parent), m_keys(keys), m_changed(changed)
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    auto *row = new QHBoxLayout;
    m_edit = new QLineEdit(this);
    m_edit->setEchoMode(QLineEdit::Password);
    m_edit->setAccessibleName(accessibleName);
    m_edit->setPlaceholderText(tr("Optional"));
    row->addWidget(m_edit, 1);
    auto *save = new QPushButton(tr("Save"), this);
    save->setAccessibleName(tr("Save %1").arg(accessibleName));
    row->addWidget(save);
    auto *remove = new QPushButton(tr("Remove"), this);
    remove->setAccessibleName(tr("Remove %1").arg(accessibleName));
    row->addWidget(remove);
    column->addLayout(row);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    column->addWidget(m_status);

    QPointer<QLabel> status = m_status;
    keys.read([status](const QString &key) {
        if (status)
            status->setText(key.isEmpty() ? tr("No key is saved.") : tr("A key is saved."));
    });

    const auto saveKey = [this] {
        const QString key = m_edit->text().trimmed();
        if (key.isEmpty())
            return;
        m_edit->clear();
        QPointer<QLabel> status = m_status;
        m_keys.write(key, [status](bool permanently) {
            if (status)
                status->setText(permanently ? tr("The key has been saved.")
                                            : tr("This computer has no credential store available, so the key will only last "
                                                 "until Papertrail closes."));
        });
        if (m_changed)
            m_changed(key);
    };
    connect(save, &QPushButton::clicked, this, saveKey);
    connect(m_edit, &QLineEdit::returnPressed, this, saveKey);
    connect(remove, &QPushButton::clicked, this, [this] {
        m_keys.clear();
        m_status->setText(tr("The key has been removed."));
        if (m_changed)
            m_changed(QString());
    });
}

} // namespace papertrail::ui
