#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QRadioButton;

namespace papertrail::ui {

// Chooses how Papertrail uses online sources, and says what that would send where. This is the only
// way lookups can start, because nothing is sent anywhere until someone has chosen here.
class OnlineLookupDialog : public QDialog {
    Q_OBJECT

public:
    // `heading` says what this is being asked for, such as adding a particular folder.
    OnlineLookupDialog(const QString &heading, const QString &mode, const QString &email, bool showEmail,
                       QWidget *parent = nullptr);

    QString mode() const; // "online_review", "online_auto" or "offline".
    void setMode(const QString &mode);
    QString email() const;

private:
    QRadioButton *m_review = nullptr;
    QRadioButton *m_auto = nullptr;
    QRadioButton *m_offline = nullptr;
    QLineEdit *m_email = nullptr;
};

} // namespace papertrail::ui
