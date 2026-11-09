#include "core/model/PublicationFields.h"

#include <array>

namespace papertrail::core {

namespace {

struct TextField {
    const char *name;
    QString Publication::*member;
};

struct NumberField {
    const char *name;
    std::optional<int> Publication::*member;
};

struct PeopleField {
    const char *name;
    QList<PersonName> Publication::*member;
};

const std::array kTextFields = {
    TextField{"title", &Publication::title},
    TextField{"subtitle", &Publication::subtitle},
    TextField{"abstract", &Publication::abstract},
    TextField{"language", &Publication::language},
    TextField{"url", &Publication::url},
    TextField{"edition", &Publication::edition},
    TextField{"series", &Publication::series},
    TextField{"doi", &Publication::doi},
    TextField{"isbn10", &Publication::isbn10},
    TextField{"isbn13", &Publication::isbn13},
    TextField{"issn", &Publication::issn},
    TextField{"eissn", &Publication::eissn},
    TextField{"arxiv_id", &Publication::arxivId},
    TextField{"pmid", &Publication::pmid},
    TextField{"container_title", &Publication::containerTitle},
    TextField{"volume", &Publication::volume},
    TextField{"issue", &Publication::issue},
    TextField{"pages", &Publication::pages},
    TextField{"publisher", &Publication::publisher},
    TextField{"place", &Publication::place},
    TextField{"conference_name", &Publication::conferenceName},
    TextField{"conference_location", &Publication::conferenceLocation},
    TextField{"institution", &Publication::institution},
    TextField{"department", &Publication::department},
    TextField{"degree", &Publication::degree},
    TextField{"report_number", &Publication::reportNumber},
    TextField{"standard_number", &Publication::standardNumber},
    TextField{"standard_body", &Publication::standardBody},
    TextField{"standard_revision", &Publication::standardRevision},
    TextField{"standard_status", &Publication::standardStatus},
    TextField{"court", &Publication::court},
    TextField{"jurisdiction", &Publication::jurisdiction},
    TextField{"neutral_citation", &Publication::neutralCitation},
    TextField{"case_number", &Publication::caseNumber},
    TextField{"case_citation_form", &Publication::caseCitationForm},
};

const std::array kNumberFields = {
    NumberField{"year", &Publication::year},
    NumberField{"month", &Publication::month},
    NumberField{"day", &Publication::day},
};

const std::array kPeopleFields = {
    PeopleField{"authors", &Publication::authors},
    PeopleField{"editors", &Publication::editors},
    PeopleField{"translators", &Publication::translators},
    PeopleField{"advisors", &Publication::advisors},
};

} // namespace

const QStringList &metadataFieldNames()
{
    static const QStringList names = [] {
        QStringList list = {QStringLiteral("type")};
        for (const TextField &field : kTextFields)
            list << QString::fromLatin1(field.name);
        for (const NumberField &field : kNumberFields)
            list << QString::fromLatin1(field.name);
        for (const PeopleField &field : kPeopleFields)
            list << QString::fromLatin1(field.name);
        return list;
    }();
    return names;
}

QString peopleToText(const QList<PersonName> &people)
{
    QStringList names;
    for (const PersonName &person : people)
        names << person.display();
    return names.join(QStringLiteral("; "));
}

QList<PersonName> peopleFromText(const QString &text)
{
    QList<PersonName> people;
    const QStringList names = text.split(u';', Qt::SkipEmptyParts);
    for (const QString &name : names) {
        PersonName person;
        person.family = name.section(u',', 0, 0).trimmed();
        person.given = name.section(u',', 1).trimmed();
        if (!person.family.isEmpty())
            people << person;
    }
    return people;
}

QString metadataFieldValue(const Publication &publication, const QString &name)
{
    if (name == QLatin1StringView("type"))
        return toDbString(publication.type);
    const QByteArray latin = name.toLatin1();
    for (const TextField &field : kTextFields) {
        if (latin == field.name)
            return publication.*field.member;
    }
    for (const NumberField &field : kNumberFields) {
        if (latin == field.name)
            return publication.*field.member ? QString::number(*(publication.*field.member)) : QString();
    }
    for (const PeopleField &field : kPeopleFields) {
        if (latin == field.name)
            return peopleToText(publication.*field.member);
    }
    return {};
}

void setMetadataFieldValue(Publication &publication, const QString &name, const QString &value)
{
    if (name == QLatin1StringView("type")) {
        if (const auto type = publicationTypeFromDb(value))
            publication.type = *type;
        return;
    }
    const QByteArray latin = name.toLatin1();
    for (const TextField &field : kTextFields) {
        if (latin == field.name) {
            publication.*field.member = value.isEmpty() ? QString() : value;
            return;
        }
    }
    for (const NumberField &field : kNumberFields) {
        if (latin == field.name) {
            publication.*field.member = value.isEmpty() ? std::nullopt : std::optional<int>(value.toInt());
            return;
        }
    }
    for (const PeopleField &field : kPeopleFields) {
        if (latin == field.name) {
            publication.*field.member = peopleFromText(value);
            return;
        }
    }
}

} // namespace papertrail::core
