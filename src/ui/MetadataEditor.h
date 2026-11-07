#pragma once

#include "core/model/Publication.h"

#include <QDialog>
#include <QHash>
#include <QList>

#include <functional>

class QComboBox;
class QCompleter;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace papertrail::ui {

// Edits one publication's metadata. Only the fields that suit its type are offered, along with any
// other field that already has a value, so nothing can be hidden and forgotten about.
class MetadataEditor : public QDialog {
    Q_OBJECT

public:
    explicit MetadataEditor(const core::Publication &publication, QWidget *parent = nullptr);

    // The publication with the dialog's values, and with its review flag cleared.
    core::Publication result() const;

    // Shows the problem and returns false if the values can't be saved.
    bool validate();

    // Values already in the library that start or contain `typed`, most used first, for a field such as
    // "container" or a people field such as "authors".
    using Suggestions = std::function<QStringList(const QString &field, const QString &typed)>;
    void setSuggestions(const Suggestions &suggestions);
    // The completer offering them for a field, for tests.
    QCompleter *completerFor(const QString &name) const;

    QComboBox *typeBox() const { return m_type; }
    QLineEdit *field(const QString &name) const;
    QPlainTextEdit *peopleField(const QString &name) const;
    bool isFieldShown(const QString &name) const;

private:
    struct TextField {
        QString name;
        QString core::Publication::*member;
        QList<core::PublicationType> suits; // Empty means every type.
        QFormLayout *form;
        QLineEdit *edit;
    };
    struct PeopleField {
        QString name;
        QList<core::PersonName> core::Publication::*member;
        QList<core::PublicationType> suits;
        QFormLayout *form;
        QPlainTextEdit *edit;
    };

    void addText(QFormLayout *form, const QString &name, const QString &label, QString core::Publication::*member,
                 const QList<core::PublicationType> &suits);
    void addPeople(QFormLayout *form, const QString &name, const QString &label,
                   QList<core::PersonName> core::Publication::*member, const QList<core::PublicationType> &suits);
    void updateVisibleFields();
    void suggestFor(QLineEdit *edit, const QString &name);
    void suggestFor(QPlainTextEdit *edit, const QString &name);

    core::Publication m_original;
    QComboBox *m_type = nullptr;
    QLineEdit *m_pageStart = nullptr;
    QLineEdit *m_pageEnd = nullptr;
    QLineEdit *m_year = nullptr;
    QPlainTextEdit *m_abstract = nullptr;
    QLabel *m_error = nullptr;
    QList<TextField> m_textFields;
    QList<PeopleField> m_peopleFields;
    Suggestions m_suggestions;
    QHash<QString, QCompleter *> m_completers;
};

} // namespace papertrail::ui
