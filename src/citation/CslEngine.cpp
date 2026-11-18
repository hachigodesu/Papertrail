#include "citation/CslEngine.h"

#include "citation/Exporters.h"

#include "quickjs.h"

#include <QCollator>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

namespace papertrail::citation {

namespace {

using Clock = std::chrono::steady_clock;

// Far more than any style needs: APA's engine, the largest, is about 150 MB, and 500 of its references 215 MB.
constexpr std::size_t kMemoryLimit = 768u * 1024 * 1024;
// Less than CslThread's stack, so QuickJS stops a deep recursion before the thread runs out.
constexpr std::size_t kScriptStack = 8u * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("CslEngine", text);
}

QByteArray resource(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// citeproc-js asks for each locale as it needs it, by its code, and gets it from the bundled ones.
JSValue retrieveLocale(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_UNDEFINED;
    const char *code = JS_ToCString(ctx, argv[0]);
    if (!code)
        return JS_EXCEPTION;
    const QByteArray xml = cslLocale(QString::fromUtf8(code));
    JS_FreeCString(ctx, code);
    if (xml.isEmpty())
        return JS_UNDEFINED;
    return JS_NewStringLen(ctx, xml.constData(), std::size_t(xml.size()));
}

QString toQString(JSContext *ctx, JSValueConst value)
{
    std::size_t length = 0;
    const char *text = JS_ToCStringLen(ctx, &length, value);
    if (!text)
        return {};
    QString result = QString::fromUtf8(text, qsizetype(length));
    JS_FreeCString(ctx, text);
    return result;
}

// citeproc-js sorts with localeCompare's numeric, case-blind comparison, which QuickJS doesn't have: it compares
// code units, putting 10 before 2 and "de Vries" after "Dent". So it's given this, through its sys object.
JSValue compareStrings(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    auto *collator = static_cast<QCollator *>(JS_GetContextOpaque(ctx));
    if (argc < 2 || !collator)
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, collator->compare(toQString(ctx, argv[0]), toQString(ctx, argv[1])));
}

} // namespace

struct CslEngine::Runtime {
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;
    Clock::time_point until;
    bool armed = false;
    bool interrupted = false;
    std::shared_ptr<std::atomic_bool> cancelled;
    QCollator collator;

    ~Runtime()
    {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt)
            JS_FreeRuntime(rt);
    }

    static int interrupt(JSRuntime *, void *opaque)
    {
        auto *self = static_cast<Runtime *>(opaque);
        if (!self->armed)
            return 0;
        if (Clock::now() > self->until || (self->cancelled && self->cancelled->load())) {
            self->interrupted = true;
            return 1;
        }
        return 0;
    }

    // What went wrong, from the exception now pending.
    QString exceptionText()
    {
        JSValue exception = JS_GetException(ctx);
        QString text;
        if (interrupted) {
            text = cancelled && cancelled->load() ? tr("It was stopped.") : tr("The style took too long, and was stopped.");
        } else if (JS_IsNull(exception) || JS_IsUndefined(exception)) {
            text = tr("The style needed more memory than it may have.");
        } else if (JS_IsError(exception)) {
            JSValue message = JS_GetPropertyStr(ctx, exception, "message");
            text = toQString(ctx, message);
            JS_FreeValue(ctx, message);
        } else {
            text = toQString(ctx, exception);
        }
        JS_FreeValue(ctx, exception);
        if (text.contains(QStringLiteral("out of memory")))
            text = tr("The style needed more memory than it may have.");
        text.remove(QStringLiteral("citeproc-js error: "));
        return text.isEmpty() ? tr("The style couldn't be used.") : text.left(500);
    }

    // Calls one of papertrail.js's functions with string or boolean arguments, within `limit`.
    std::optional<QString> call(const char *function, const QList<QVariant> &arguments, std::chrono::milliseconds limit,
                                QString *error)
    {
        JS_UpdateStackTop(rt);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue target = JS_GetPropertyStr(ctx, global, function);
        QList<JSValue> values;
        for (const QVariant &argument : arguments) {
            if (argument.typeId() == QMetaType::Bool) {
                values << JS_NewBool(ctx, argument.toBool());
            } else {
                const QByteArray text = argument.toString().toUtf8();
                values << JS_NewStringLen(ctx, text.constData(), std::size_t(text.size()));
            }
        }
        interrupted = false;
        until = Clock::now() + limit;
        armed = true;
        JSValue result = JS_Call(ctx, target, global, int(values.size()), values.data());
        armed = false;
        for (JSValue value : values)
            JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, target);
        JS_FreeValue(ctx, global);
        if (JS_IsException(result)) {
            if (error)
                *error = exceptionText();
            return std::nullopt;
        }
        QString text = toQString(ctx, result);
        JS_FreeValue(ctx, result);
        return text;
    }
};

CslEngine::CslEngine() = default;

CslEngine::~CslEngine() = default;

void CslEngine::setLimits(std::chrono::milliseconds base, std::chrono::milliseconds perItem)
{
    m_base = base;
    m_perItem = perItem;
}

void CslEngine::setCancelFlag(std::shared_ptr<std::atomic_bool> cancelled)
{
    m_cancelled = std::move(cancelled);
    if (m_runtime)
        m_runtime->cancelled = m_cancelled;
}

void CslEngine::discardRuntime()
{
    m_runtime.reset();
    m_styleKey.clear();
}

bool CslEngine::ensureRuntime(QString *error)
{
    if (m_runtime)
        return true;
    const QByteArray citeproc = resource(QStringLiteral(":/csl/citeproc.js"));
    const QByteArray driver = resource(QStringLiteral(":/csl/papertrail.js"));
    if (citeproc.isEmpty() || driver.isEmpty()) {
        if (error)
            *error = tr("This copy of Papertrail is missing citeproc-js.");
        return false;
    }
    auto runtime = std::make_unique<Runtime>();
    runtime->rt = JS_NewRuntime();
    if (!runtime->rt) {
        if (error)
            *error = tr("The style engine couldn't be started.");
        return false;
    }
    JS_SetMemoryLimit(runtime->rt, kMemoryLimit);
    JS_SetMaxStackSize(runtime->rt, kScriptStack);
    JS_SetInterruptHandler(runtime->rt, &Runtime::interrupt, runtime.get());
    runtime->cancelled = m_cancelled;
    runtime->ctx = JS_NewContext(runtime->rt);
    if (!runtime->ctx) {
        if (error)
            *error = tr("The style engine couldn't be started.");
        return false;
    }
    JSValue global = JS_GetGlobalObject(runtime->ctx);
    JS_SetContextOpaque(runtime->ctx, &runtime->collator);
    JS_SetPropertyStr(runtime->ctx, global, "__ptLocale", JS_NewCFunction(runtime->ctx, &retrieveLocale, "__ptLocale", 1));
    JS_SetPropertyStr(runtime->ctx, global, "__ptCompare", JS_NewCFunction(runtime->ctx, &compareStrings, "__ptCompare", 2));
    JS_FreeValue(runtime->ctx, global);

    const QByteArray program = QByteArrayLiteral("var module = {exports: {}};\n") + citeproc + '\n' + driver;
    JS_UpdateStackTop(runtime->rt);
    runtime->until = Clock::now() + m_base;
    runtime->armed = true;
    JSValue loaded = JS_Eval(runtime->ctx, program.constData(), std::size_t(program.size()), "citeproc.js", JS_EVAL_TYPE_GLOBAL);
    runtime->armed = false;
    if (JS_IsException(loaded)) {
        const QString why = runtime->exceptionText();
        if (error)
            *error = tr("citeproc-js couldn't be loaded: %1").arg(why);
        return false;
    }
    JS_FreeValue(runtime->ctx, loaded);
    m_runtime = std::move(runtime);
    return true;
}

bool CslEngine::useStyle(const CslLayout &layout, QString *error)
{
    const QString key = layout.style.id + u'\n' + layout.locale + (layout.forceLocale ? u'!' : u' ')
        + QString::number(qHash(layout.xml));
    if (m_runtime && key == m_styleKey)
        return true;
    const std::optional<CslStyleInfo> independent = readCslStyle(layout.xml, error);
    if (!independent)
        return false;
    if (!ensureRuntime(error))
        return false;
    m_styleKey.clear();
    // Sorted as the locale the style is shown in would sort.
    QCollator collator(QLocale(QString(layout.forceLocale || independent->defaultLocale.isEmpty() ? layout.locale : independent->defaultLocale)
                                   .replace(u'-', u'_')));
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setIgnorePunctuation(true);
    m_runtime->collator = collator;
    const std::optional<QString> made = m_runtime->call(
        "ptSetStyle", {QString::fromUtf8(layout.xml), layout.locale, layout.forceLocale}, m_base, error);
    if (!made) {
        if (m_runtime->interrupted)
            discardRuntime();
        return false;
    }
    m_styleKey = key;
    m_isNote = independent->isNote;
    return true;
}

namespace {

QByteArray itemsJson(const QList<CitationItem> &items, const QString &prefix, const StyleOptions &options)
{
    QJsonArray array;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const CitationItem &item = items.at(i);
        // Padded, since citeproc-js orders some things by id as text.
        QJsonObject object = cslJsonItem(item, prefix + QStringLiteral("%1").arg(i, 5, 10, QLatin1Char(u'0')));
        // Nothing a style shows, and the abstract can be long.
        object.remove(QStringLiteral("abstract"));
        object.remove(QStringLiteral("keyword"));
        // A page on the web, rather than a work with a DOI, says when it was looked at, as the Harvard style does.
        if (!item.url.isEmpty() && item.doi.isEmpty() && options.accessed.isValid()) {
            object.insert(QStringLiteral("accessed"),
                          QJsonObject{{QStringLiteral("date-parts"),
                                       QJsonArray{QJsonArray{options.accessed.year(), options.accessed.month(), options.accessed.day()}}}});
        }
        array.append(object);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

} // namespace

std::optional<FormattedReferences> CslEngine::references(const QList<CitationItem> &items, const StyleOptions &options, QString *error)
{
    if (!m_runtime || m_styleKey.isEmpty()) {
        if (error)
            *error = tr("No style has been chosen.");
        return std::nullopt;
    }
    FormattedReferences result;
    if (items.isEmpty())
        return result;
    const QString prefix = QStringLiteral("r%1-").arg(++m_call);
    const std::chrono::milliseconds limit = m_base + m_perItem * items.size();
    const std::optional<QString> answer
        = m_runtime->call("ptBibliography", {QString::fromUtf8(itemsJson(items, prefix, options))}, limit, error);
    if (!answer) {
        if (m_runtime->interrupted)
            discardRuntime();
        else
            m_styleKey.clear(); // Made again next time, in case the failure left it in a bad state.
        return std::nullopt;
    }
    const QJsonObject parsed = QJsonDocument::fromJson(answer->toUtf8()).object();
    if (parsed.value(QStringLiteral("none")).toBool()) {
        // A style with no reference list, such as some note styles: each is listed as its note.
        for (qsizetype i = 0; i < items.size(); ++i) {
            std::optional<RichText> one = citation({items.at(i)}, options, error);
            if (!one)
                return std::nullopt;
            result.entries << (items.at(i).kind == CitationItem::Kind::Case ? caseCitation(items.at(i)) : *one);
            result.order << int(i);
        }
        return result;
    }
    const QJsonArray ids = parsed.value(QStringLiteral("ids")).toArray();
    const QJsonArray entries = parsed.value(QStringLiteral("entries")).toArray();
    for (qsizetype i = 0; i < ids.size() && i < entries.size(); ++i) {
        const QString id = ids.at(i).toString();
        bool ok = false;
        const int index = id.startsWith(prefix) ? id.mid(prefix.size()).toInt(&ok) : -1;
        if (!ok || index < 0 || index >= items.size())
            continue;
        RichText margin;
        RichText body = fromMarked(entries.at(i).toString(), &margin);
        // A case is cited in its legal form in every style, keeping the style's number for it.
        if (items.at(index).kind == CitationItem::Kind::Case)
            body = caseCitation(items.at(index));
        RichText entry;
        if (!margin.isEmpty())
            entry.append(margin).add(QStringLiteral(" "));
        entry.append(body);
        result.entries << entry;
        result.order << index;
    }
    return result;
}

std::optional<RichText> CslEngine::citation(const QList<CitationItem> &items, const StyleOptions &options, QString *error)
{
    if (!m_runtime || m_styleKey.isEmpty()) {
        if (error)
            *error = tr("No style has been chosen.");
        return std::nullopt;
    }
    if (items.isEmpty())
        return RichText();
    const QString prefix = QStringLiteral("c%1-").arg(++m_call);
    const std::chrono::milliseconds limit = m_base + m_perItem * items.size();
    const std::optional<QString> answer
        = m_runtime->call("ptCitation", {QString::fromUtf8(itemsJson(items, prefix, options)), m_isNote}, limit, error);
    if (!answer) {
        if (m_runtime->interrupted)
            discardRuntime();
        else
            m_styleKey.clear();
        return std::nullopt;
    }
    return fromMarked(*answer);
}

RichText CslEngine::fromMarked(const QString &marked, RichText *margin)
{
    RichText body;
    RichText *target = &body;
    QList<RichText::Run> styles{RichText::Run{}};
    RichText::Run run;
    const auto flush = [&] {
        if (!run.text.isEmpty())
            target->add(run);
        run.text.clear();
    };
    for (qsizetype i = 0; i < marked.size(); ++i) {
        const char16_t c = marked.at(i).unicode();
        if (c == 0xE000 && i + 1 < marked.size()) {
            flush();
            RichText::Run next = styles.last();
            switch (marked.at(++i).unicode()) {
            case u'i': next.italic = true; break;
            case u'I': next.italic = false; break;
            case u'b': next.bold = true; break;
            case u'B': next.bold = false; break;
            case u'c': next.smallCaps = true; break;
            case u'C': next.smallCaps = false; break;
            case u'u': next.position = RichText::Position::Superscript; break;
            case u'd': next.position = RichText::Position::Subscript; break;
            case u'n': next.position = RichText::Position::Baseline; break;
            default: break;
            }
            styles << next;
        } else if (c == 0xE001) {
            flush();
            if (styles.size() > 1)
                styles.removeLast();
        } else if (c == 0xE002) {
            flush();
            if (margin)
                target = margin;
        } else if (c == 0xE003) {
            flush();
            target = &body;
        } else if (c >= 0xE000 && c <= 0xE0FF) {
            // Nothing else is used.
        } else {
            const RichText::Run &style = styles.last();
            if (!run.sameStyle(style)) {
                flush();
                run = style;
                run.text.clear();
            }
            run.text += marked.at(i);
        }
    }
    flush();
    return body;
}

} // namespace papertrail::citation
