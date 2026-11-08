#pragma once

#include "core/model/Publication.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace papertrail::ui {

class LibrarySession;

// Confirms summaries in a batch at half price: what's sent, which are left out, and what it may cost, worked out
// here without asking Anthropic. Nothing is sent until the user says so.
class BatchSummaryDialog : public QDialog {
    Q_OBJECT

public:
    BatchSummaryDialog(LibrarySession &session, const QList<qint64> &publicationIds, QWidget *parent = nullptr);

    // What would be sent as things stand: those without a summary, and those with one if replacing is chosen.
    QList<core::Publication> included() const;
    QString model() const;
    QString effort() const;

    QComboBox *modelBox() const { return m_model; }
    QComboBox *effortBox() const { return m_effort; }
    QCheckBox *replaceBox() const { return m_replace; }
    QLabel *estimateLabel() const { return m_estimate; }
    QPushButton *sendButton() const { return m_send; }

    // The most sent in one go, to keep a batch's cost within reason.
    static constexpr int kMostAtOnce = 500;

private:
    void update();

    LibrarySession &m_session;
    QList<core::Publication> m_without; // No summary yet.
    QList<core::Publication> m_with;    // Already have one, which would be replaced.
    int m_pending = 0; // Left out, since a summary is on its way or waiting to be reviewed.
    QComboBox *m_model = nullptr;
    QComboBox *m_effort = nullptr;
    QCheckBox *m_replace = nullptr;
    QLabel *m_estimate = nullptr;
    QPushButton *m_send = nullptr;
};

} // namespace papertrail::ui
