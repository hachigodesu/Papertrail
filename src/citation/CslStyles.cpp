#include "citation/CslStyles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

namespace papertrail::citation {

namespace {

const QString kCslNamespace = QStringLiteral("http://purl.org/net/xbiblio/csl");
const QString kStylesFolder = QStringLiteral(":/csl/styles");
const QString kLocalesFolder = QStringLiteral(":/csl/locales");

QString tr(const char *text)
{
    return QCoreApplication::translate("CslStyles", text);
}

QByteArray readResource(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

std::optional<CslStyleInfo> readCslStyle(const QByteArray &xml, QString *error)
{
    const auto fail = [error](const QString &why) -> std::optional<CslStyleInfo> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (xml.size() > kMaxCslStyleBytes)
        return fail(tr("It's larger than any style should be."));

    QXmlStreamReader reader(xml);
    CslStyleInfo info;
    bool hasCitation = false;
    int depth = 0;
    bool inInfo = false;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::DTD)
            return fail(tr("It declares a DTD, which a style never needs."));
        if (token == QXmlStreamReader::EntityReference)
            return fail(tr("It uses entities, which a style never needs."));
        if (token == QXmlStreamReader::EndElement) {
            --depth;
            if (depth == 1)
                inInfo = false;
            continue;
        }
        if (token != QXmlStreamReader::StartElement)
            continue;
        ++depth;
        const QStringView name = reader.name();
        if (depth == 1) {
            if (name != u"style" || reader.namespaceUri() != kCslNamespace)
                return fail(tr("It isn't a CSL style."));
            info.isNote = reader.attributes().value(QStringLiteral("class")) == u"note";
            info.defaultLocale = reader.attributes().value(QStringLiteral("default-locale")).toString().trimmed();
            continue;
        }
        if (reader.namespaceUri() != kCslNamespace)
            continue;
        if (depth == 2) {
            if (name == u"info")
                inInfo = true;
            else if (name == u"citation")
                hasCitation = true;
            else if (name == u"bibliography")
                info.hasBibliography = true;
            continue;
        }
        if (depth != 3 || !inInfo)
            continue;
        if (name == u"id") {
            info.id = reader.readElementText().trimmed();
            --depth;
        } else if (name == u"title") {
            info.title = reader.readElementText().simplified();
            --depth;
        } else if (name == u"title-short") {
            info.shortTitle = reader.readElementText().simplified();
            --depth;
        } else if (name == u"link") {
            if (reader.attributes().value(QStringLiteral("rel")) == u"independent-parent")
                info.parentId = reader.attributes().value(QStringLiteral("href")).toString().trimmed();
        } else if (name == u"category") {
            const QStringView format = reader.attributes().value(QStringLiteral("citation-format"));
            if (!format.isEmpty())
                info.citationFormat = format.toString();
            const QStringView field = reader.attributes().value(QStringLiteral("field"));
            if (!field.isEmpty() && !info.fields.contains(field.toString()))
                info.fields << field.toString();
        }
    }
    if (reader.hasError())
        return fail(tr("It isn't well-formed XML: %1 (line %2).").arg(reader.errorString()).arg(reader.lineNumber()));
    if (info.id.isEmpty())
        return fail(tr("It has no id."));
    if (info.title.isEmpty())
        return fail(tr("It has no title."));
    if (info.parentId == info.id)
        return fail(tr("It names itself as its parent."));
    if (!info.isDependent() && !hasCitation)
        return fail(tr("It has no layout for citations, and no parent style to take one from."));
    if (info.citationFormat.isEmpty() && info.isNote)
        info.citationFormat = QStringLiteral("note");
    return info;
}

namespace {

struct Bundled {
    QList<CslStyleInfo> styles;
    QHash<QString, QString> files; // By id.
};

const Bundled &bundled()
{
    static const Bundled all = [] {
        Bundled found;
        const QStringList names = QDir(kStylesFolder).entryList({QStringLiteral("*.csl")}, QDir::Files, QDir::Name);
        for (const QString &name : names) {
            const QString path = kStylesFolder + u'/' + name;
            std::optional<CslStyleInfo> info = readCslStyle(readResource(path));
            if (!info)
                continue;
            info->bundled = true;
            found.files.insert(info->id, path);
            found.styles << *info;
        }
        std::sort(found.styles.begin(), found.styles.end(), [](const CslStyleInfo &a, const CslStyleInfo &b) {
            return QString::localeAwareCompare(a.title, b.title) < 0;
        });
        return found;
    }();
    return all;
}

struct Locales {
    QList<CslLocale> list;
    QHash<QString, QString> primary; // "de" to "de-DE".
};

const Locales &locales()
{
    static const Locales all = [] {
        Locales found;
        const QJsonObject index = QJsonDocument::fromJson(readResource(kLocalesFolder + QStringLiteral("/locales.json"))).object();
        const QJsonObject primary = index.value(QStringLiteral("primary-dialects")).toObject();
        for (auto it = primary.constBegin(); it != primary.constEnd(); ++it)
            found.primary.insert(it.key(), it.value().toString());
        const QJsonObject names = index.value(QStringLiteral("language-names")).toObject();
        const QStringList files = QDir(kLocalesFolder).entryList({QStringLiteral("locales-*.xml")}, QDir::Files, QDir::Name);
        for (const QString &file : files) {
            const QString code = file.mid(8, file.size() - 12);
            const QJsonArray name = names.value(code).toArray();
            found.list << CslLocale{code, name.isEmpty() ? code : name.first().toString()};
        }
        return found;
    }();
    return all;
}

} // namespace

const QList<CslStyleInfo> &bundledCslStyles()
{
    return bundled().styles;
}

QByteArray bundledCslStyle(const QString &id)
{
    const QString path = bundled().files.value(id);
    return path.isEmpty() ? QByteArray() : readResource(path);
}

const QList<CslLocale> &cslLocales()
{
    return locales().list;
}

QByteArray cslLocale(const QString &code)
{
    // Only letters and hyphens, so nothing can reach another folder.
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$"));
    if (!valid.match(code).hasMatch())
        return {};
    const auto file = [](const QString &which) { return readResource(kLocalesFolder + QStringLiteral("/locales-") + which + QStringLiteral(".xml")); };
    QByteArray xml = file(code);
    if (!xml.isEmpty())
        return xml;
    const QString language = code.section(u'-', 0, 0).toLower();
    const QString dialect = locales().primary.value(language);
    if (!dialect.isEmpty())
        xml = file(dialect);
    if (!xml.isEmpty())
        return xml;
    for (const CslLocale &locale : cslLocales()) {
        if (locale.code.section(u'-', 0, 0).toLower() == language)
            return file(locale.code);
    }
    return {};
}

bool isCslStyleKey(const QString &key)
{
    return key.startsWith(QStringLiteral("csl:")) && key.size() > 4;
}

QString cslStyleKey(const QString &id)
{
    return QStringLiteral("csl:") + id;
}

QString cslStyleId(const QString &key)
{
    return isCslStyleKey(key) ? key.mid(4) : QString();
}

std::optional<CslLayout> resolveCslStyle(const QString &id, const QString &locale, const AddedCslStyles &added, QString *error)
{
    const auto fail = [error](const QString &why) -> std::optional<CslLayout> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    const auto find = [&](const QString &which) {
        QByteArray xml = bundledCslStyle(which);
        if (xml.isEmpty() && added)
            xml = added(which);
        return xml;
    };
    QByteArray xml = find(id);
    if (xml.isEmpty())
        return fail(tr("The style %1 isn't in this library.").arg(id));
    QString why;
    std::optional<CslStyleInfo> chosen = readCslStyle(xml, &why);
    if (!chosen)
        return fail(why);
    chosen->bundled = !bundledCslStyle(id).isEmpty();

    CslLayout layout;
    layout.style = *chosen;
    layout.xml = xml;
    if (chosen->isDependent()) {
        layout.xml = find(chosen->parentId);
        if (layout.xml.isEmpty())
            return fail(tr("It uses the layout of another style, %1, which isn't here. Add its parent style first.").arg(chosen->parentId));
        const std::optional<CslStyleInfo> parent = readCslStyle(layout.xml, &why);
        if (!parent)
            return fail(why);
        if (parent->isDependent())
            return fail(tr("Its parent style is itself dependent on another."));
    }
    if (!locale.trimmed().isEmpty()) {
        layout.locale = locale.trimmed();
        layout.forceLocale = true;
    } else if (chosen->isDependent() && !chosen->defaultLocale.isEmpty()) {
        layout.locale = chosen->defaultLocale;
        layout.forceLocale = true;
    } else {
        layout.locale = QStringLiteral("en-US");
    }
    return layout;
}

} // namespace papertrail::citation
