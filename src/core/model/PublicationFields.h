#pragma once

#include "core/model/Publication.h"

#include <QString>
#include <QStringList>

namespace papertrail::core {

// A publication's metadata as named text values, so that it can be compared field by field, kept as
// earlier values, and put back. The names are the same ones the provenance table uses. Reading status,
// rating and notes aren't included, because those are the reader's own and not metadata.
const QStringList &metadataFieldNames();
QString metadataFieldValue(const Publication &publication, const QString &name);
void setMetadataFieldValue(Publication &publication, const QString &name, const QString &value);

// What a person-name field is stored as: "Family, Given; Family, Given".
QString peopleToText(const QList<PersonName> &people);
QList<PersonName> peopleFromText(const QString &text);

} // namespace papertrail::core
