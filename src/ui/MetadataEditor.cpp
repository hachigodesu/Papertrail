#include "ui/MetadataEditor.h"

#include "import/Extractors.h"
#include "ui/Labels.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace papertrail::ui {

using namespace core;

using T = PublicationType;

namespace {

QString peopleToLines(const QList<PersonName> &people)
{
    QStringList lines;
    for (const PersonName &person : people)
        lines << person.display();
    return lines.join(u'\n');
}

QList<PersonName> linesToPeople(const QString &text)
{
    QList<PersonName> people;
    for (const QString &line : text.split(u'\n', Qt::SkipEmptyParts)) {
        const QString name = line.simplified();
        if (!name.isEmpty())
            people << import::parsePersonName(name);
    }
    return people;
}

QWidget *scrollable(QWidget *contents)
{
    auto *area = new QScrollArea;
    area->setWidget(contents);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

} // namespace

MetadataEditor::MetadataEditor(const Publication &publication, QWidget *parent)
    : QDialog(parent), m_original(publication)
{
    setWindowTitle(tr("Edit metadata"));
    resize(620, 640);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);

    auto *mainPage = new QWidget;
    auto *main = new QFormLayout(mainPage);
    main->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_type = new QComboBox(mainPage);
    for (const PublicationType type : allPublicationTypes())
        m_type->addItem(typeLabel(type), int(type));
    m_type->setCurrentIndex(m_type->findData(int(publication.type)));
    main->addRow(tr("Type"), m_type);

    addText(main, QStringLiteral("title"), tr("Title"), &Publication::title, {});
    addText(main, QStringLiteral("subtitle"), tr("Subtitle"), &Publication::subtitle, {});
    addPeople(main, QStringLiteral("authors"), tr("Authors\n(one per line)"), &Publication::authors, {});
    addPeople(main, QStringLiteral("editors"), tr("Editors"), &Publication::editors,
              {T::JournalVolume, T::ConferenceProceedings, T::Book, T::Textbook, T::BookChapter});
    addPeople(main, QStringLiteral("translators"), tr("Translators"), &Publication::translators,
              {T::Book, T::Textbook, T::BookChapter});
    addPeople(main, QStringLiteral("advisors"), tr("Advisors"), &Publication::advisors, {T::Thesis, T::Dissertation});

    m_year = new QLineEdit(mainPage);
    m_year->setValidator(new QIntValidator(0, 9999, m_year));
    m_year->setMaximumWidth(90);
    if (publication.year)
        m_year->setText(QString::number(*publication.year));
    main->addRow(tr("Year"), m_year);

    const QList<PublicationType> inContainer = {T::JournalVolume, T::JournalArticle, T::ConferencePaper, T::BookChapter,
                                                T::Preprint};
    const QList<PublicationType> bookLike = {T::Textbook, T::Book, T::BookChapter, T::ConferenceProceedings};
    const QList<PublicationType> published = {T::JournalVolume, T::ConferenceProceedings, T::Textbook, T::Book,
                                              T::BookChapter, T::TechnicalReport, T::Standard};

    addText(main, QStringLiteral("container"), tr("Journal or source"), &Publication::containerTitle, inContainer);
    addText(main, QStringLiteral("volume"), tr("Volume"), &Publication::volume, inContainer);
    addText(main, QStringLiteral("issue"), tr("Issue"), &Publication::issue, {T::JournalVolume, T::JournalArticle});
    addText(main, QStringLiteral("pages"), tr("Pages"), &Publication::pages, inContainer);
    addText(main, QStringLiteral("publisher"), tr("Publisher"), &Publication::publisher, published);
    addText(main, QStringLiteral("place"), tr("Place"), &Publication::place, bookLike);
    addText(main, QStringLiteral("edition"), tr("Edition"), &Publication::edition, bookLike);
    addText(main, QStringLiteral("series"), tr("Series"), &Publication::series, bookLike);
    addText(main, QStringLiteral("conference"), tr("Conference"), &Publication::conferenceName,
            {T::ConferencePaper, T::ConferenceProceedings});
    addText(main, QStringLiteral("conference_location"), tr("Conference location"), &Publication::conferenceLocation,
            {T::ConferencePaper, T::ConferenceProceedings});
    addText(main, QStringLiteral("institution"), tr("Institution"), &Publication::institution,
            {T::Thesis, T::Dissertation, T::TechnicalReport});
    addText(main, QStringLiteral("department"), tr("Department"), &Publication::department, {T::Thesis, T::Dissertation});
    addText(main, QStringLiteral("degree"), tr("Degree"), &Publication::degree, {T::Thesis, T::Dissertation});
    addText(main, QStringLiteral("report_number"), tr("Report number"), &Publication::reportNumber, {T::TechnicalReport});
    addText(main, QStringLiteral("standard_number"), tr("Standard number"), &Publication::standardNumber, {T::Standard});
    addText(main, QStringLiteral("standard_body"), tr("Issuing body"), &Publication::standardBody, {T::Standard});
    addText(main, QStringLiteral("standard_revision"), tr("Revision"), &Publication::standardRevision, {T::Standard});
    addText(main, QStringLiteral("standard_status"), tr("Status"), &Publication::standardStatus, {T::Standard});
    tabs->addTab(scrollable(mainPage), tr("Details"));

    auto *idPage = new QWidget;
    auto *ids = new QFormLayout(idPage);
    ids->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    addText(ids, QStringLiteral("doi"), tr("DOI"), &Publication::doi, {});
    addText(ids, QStringLiteral("isbn13"), tr("ISBN-13"), &Publication::isbn13, {});
    addText(ids, QStringLiteral("isbn10"), tr("ISBN-10"), &Publication::isbn10, {});
    addText(ids, QStringLiteral("issn"), tr("ISSN"), &Publication::issn, {});
    addText(ids, QStringLiteral("eissn"), tr("eISSN"), &Publication::eissn, {});
    addText(ids, QStringLiteral("arxiv"), tr("arXiv ID"), &Publication::arxivId, {});
    addText(ids, QStringLiteral("pmid"), tr("PubMed ID"), &Publication::pmid, {});
    addText(ids, QStringLiteral("url"), tr("Web address"), &Publication::url, {});
    addText(ids, QStringLiteral("language"), tr("Language"), &Publication::language, {});
    tabs->addTab(scrollable(idPage), tr("Identifiers"));

    m_abstract = new QPlainTextEdit(publication.abstract, this);
    m_abstract->setAccessibleName(tr("Abstract"));
    tabs->addTab(m_abstract, tr("Abstract"));

    m_error = new QLabel(this);
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (validate())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_type, &QComboBox::currentIndexChanged, this, [this] { updateVisibleFields(); });
    updateVisibleFields();
}

void MetadataEditor::addText(QFormLayout *form, const QString &name, const QString &label,
                             QString Publication::*member, const QList<PublicationType> &suits)
{
    auto *edit = new QLineEdit(m_original.*member);
    form->addRow(label, edit);
    m_textFields << TextField{name, member, suits, form, edit};
}

void MetadataEditor::addPeople(QFormLayout *form, const QString &name, const QString &label,
                               QList<PersonName> Publication::*member, const QList<PublicationType> &suits)
{
    auto *edit = new QPlainTextEdit(peopleToLines(m_original.*member));
    edit->setTabChangesFocus(true);
    edit->setMaximumHeight(90);
    edit->setPlaceholderText(tr("Family, Given"));
    form->addRow(label, edit);
    m_peopleFields << PeopleField{name, member, suits, form, edit};
}

void MetadataEditor::updateVisibleFields()
{
    const auto type = PublicationType(m_type->currentData().toInt());
    for (const TextField &field : m_textFields) {
        field.form->setRowVisible(field.edit,
                                  field.suits.isEmpty() || field.suits.contains(type) || !field.edit->text().isEmpty());
    }
    for (const PeopleField &field : m_peopleFields) {
        field.form->setRowVisible(field.edit, field.suits.isEmpty() || field.suits.contains(type)
                                                  || !field.edit->toPlainText().isEmpty());
    }
}

QLineEdit *MetadataEditor::field(const QString &name) const
{
    for (const TextField &field : m_textFields) {
        if (field.name == name)
            return field.edit;
    }
    return nullptr;
}

QPlainTextEdit *MetadataEditor::peopleField(const QString &name) const
{
    for (const PeopleField &field : m_peopleFields) {
        if (field.name == name)
            return field.edit;
    }
    return nullptr;
}

bool MetadataEditor::isFieldShown(const QString &name) const
{
    for (const TextField &field : m_textFields) {
        if (field.name == name)
            return field.form->isRowVisible(field.edit);
    }
    for (const PeopleField &field : m_peopleFields) {
        if (field.name == name)
            return field.form->isRowVisible(field.edit);
    }
    return false;
}

bool MetadataEditor::validate()
{
    if (field(QStringLiteral("title"))->text().trimmed().isEmpty()) {
        m_error->setText(tr("A publication needs a title."));
        m_error->show();
        field(QStringLiteral("title"))->setFocus();
        return false;
    }
    m_error->hide();
    return true;
}

Publication MetadataEditor::result() const
{
    Publication p = m_original;
    p.type = PublicationType(m_type->currentData().toInt());
    for (const TextField &field : m_textFields) {
        const QString value = field.edit->text().trimmed();
        p.*field.member = value.isEmpty() ? QString() : value;
    }
    for (const PeopleField &field : m_peopleFields)
        p.*field.member = linesToPeople(field.edit->toPlainText());

    const QString abstract = m_abstract->toPlainText().trimmed();
    p.abstract = abstract.isEmpty() ? QString() : abstract;

    const QString year = m_year->text().trimmed();
    const std::optional<int> newYear = year.isEmpty() ? std::nullopt : std::optional<int>(year.toInt());
    if (newYear != m_original.year) {
        // A month and day belonged to the old year.
        p.year = newYear;
        p.month.reset();
        p.day.reset();
    }
    p.needsReview = false;
    return p;
}

} // namespace papertrail::ui
