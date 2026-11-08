#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
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

    // Whether to check now and then for retractions. Only offered with the email, in the settings themselves.
    void setCheckNotices(bool check);
    bool checkNotices() const;
    QCheckBox *checkNoticesBox() const { return m_notices; }

private:
    QRadioButton *m_review = nullptr;
    QRadioButton *m_auto = nullptr;
    QRadioButton *m_offline = nullptr;
    QLineEdit *m_email = nullptr;
    QCheckBox *m_notices = nullptr;
};

} // namespace papertrail::ui
