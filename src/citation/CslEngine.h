#pragma once

#include "citation/CitationItem.h"
#include "citation/CslStyles.h"
#include "citation/Formatting.h"
#include "citation/Styles.h"

#include <QList>
#include <QString>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

namespace papertrail::citation {

// Renders CSL styles with citeproc-js, run in QuickJS-ng with no access to anything but what it's handed. One engine
// is kept for the last style used, since making one can take seconds for a large style such as APA.
//
// It isn't thread-safe, and needs a deep stack: use it on one thread at a time, made with at least
// kCslStackBytes of stack, such as CslThread's.
class CslEngine {
public:
    static constexpr std::size_t kCslStackBytes = 16 * 1024 * 1024;

    CslEngine();
    ~CslEngine();
    CslEngine(const CslEngine &) = delete;
    CslEngine &operator=(const CslEngine &) = delete;

    // How long a style may take: making its engine, and each list, which takes longer the more it holds. A style that
    // loops on itself is stopped then. Tests shorten it.
    void setLimits(std::chrono::milliseconds base, std::chrono::milliseconds perItem);
    // Checked while rendering, so a newer request can stop an older one. It stops with an error.
    void setCancelFlag(std::shared_ptr<std::atomic_bool> cancelled);

    // Gets ready for a style, keeping the engine if it's the one already in use.
    bool useStyle(const CslLayout &layout, QString *error = nullptr);

    // A reference list in the style's order, numbered by it if it numbers them. A case's entry is in its own legal
    // form (caseCitation), as it is in every style. `inText` is left empty: see citation().
    std::optional<FormattedReferences> references(const QList<CitationItem> &items, const StyleOptions &options = {},
                                                  QString *error = nullptr);
    // One citation of all these together, or for a note style, its note.
    std::optional<RichText> citation(const QList<CitationItem> &items, const StyleOptions &options = {},
                                     QString *error = nullptr);

    // Turns citeproc's marked text into runs, and sets `margin` to a numeric style's label such as "[1]".
    static RichText fromMarked(const QString &marked, RichText *margin = nullptr);

private:
    struct Runtime;
    bool ensureRuntime(QString *error);
    void discardRuntime();

    std::unique_ptr<Runtime> m_runtime;
    QString m_styleKey;
    bool m_isNote = false;
    std::chrono::milliseconds m_base{60000};
    std::chrono::milliseconds m_perItem{100};
    std::shared_ptr<std::atomic_bool> m_cancelled;
    qint64 m_call = 0;
};

} // namespace papertrail::citation
