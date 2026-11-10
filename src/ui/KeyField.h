#pragma once

#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;

namespace papertrail::ui {

class ApiKeyStore;

// Enters, replaces or removes one key of the user's own, kept in `keys`, saying whether one is saved without ever
// showing it. `changed` is told the new key, or an empty one once it's removed.
class KeyField : public QWidget {
    Q_OBJECT

public:
    KeyField(ApiKeyStore &keys, const QString &accessibleName, const std::function<void(const QString &)> &changed,
             QWidget *parent = nullptr);

private:
    ApiKeyStore &m_keys;
    std::function<void(const QString &)> m_changed;
    QLineEdit *m_edit = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace papertrail::ui
