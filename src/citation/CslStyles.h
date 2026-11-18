#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace papertrail::citation {

// What a CSL style says about itself, read from its XML by readCslStyle.
struct CslStyleInfo {
    QString id;    // Its info/id, a URI such as "http://www.zotero.org/styles/apa".
    QString title; // As shown, such as "American Psychological Association 7th edition".
    QString shortTitle;
    QString parentId;       // For a dependent style, the id of the independent style whose layout it uses.
    QString defaultLocale;  // Its default-locale, such as "en-GB", if it has one.
    QString citationFormat; // "author-date", "numeric", "note", "label" or "author".
    QStringList fields;     // Such as "medicine" or "engineering".
    bool isNote = false;    // A note style, whose in-text citation is a footnote.
    bool hasBibliography = false;
    bool bundled = false;   // Comes with Papertrail, rather than added to this library from a file.

    bool isDependent() const { return !parentId.isEmpty(); }
};

// The most a style file may be, which is far more than any real one.
inline constexpr qsizetype kMaxCslStyleBytes = 1024 * 1024;

// Checks that this is a CSL style worth giving citeproc-js, which renders a broken style without complaint: at most
// kMaxCslStyleBytes, well-formed XML with no DTD, a <style> in CSL's namespace with an id and a title, and either
// a citation layout of its own or a parent. Returns what it says about itself, or nothing with `error` set.
std::optional<CslStyleInfo> readCslStyle(const QByteArray &xml, QString *error = nullptr);

// The styles that come with Papertrail, by title.
const QList<CslStyleInfo> &bundledCslStyles();
// A bundled style's XML, or nothing if no bundled style has this id.
QByteArray bundledCslStyle(const QString &id);

// A CSL locale's XML, such as for "de-DE" or just "de", or nothing if there's none like it.
QByteArray cslLocale(const QString &code);
struct CslLocale {
    QString code; // "de-DE"
    QString name; // "Deutsch (Deutschland)", in its own language
};
const QList<CslLocale> &cslLocales();

// The citation.style setting holds one of the hand-written styles' keys, or "csl:" and a CSL style's id.
bool isCslStyleKey(const QString &key);
QString cslStyleKey(const QString &id);
QString cslStyleId(const QString &key);

// A style ready to render: the layout to give citeproc-js (a dependent style's parent's), and the locale.
struct CslLayout {
    CslStyleInfo style;    // The one chosen, which may be dependent.
    QByteArray xml;        // The independent style's XML.
    QString locale;
    bool forceLocale = false; // Whether the locale wins over the layout's own default-locale.
};

// Finds a style by its id among the bundled styles, then those `added` returns (the library's own), and its parent if
// it's dependent. `locale` is the citation.locale setting, where empty means the style's own.
using AddedCslStyles = std::function<QByteArray(const QString &id)>;
std::optional<CslLayout> resolveCslStyle(const QString &id, const QString &locale, const AddedCslStyles &added,
                                         QString *error = nullptr);

} // namespace papertrail::citation
