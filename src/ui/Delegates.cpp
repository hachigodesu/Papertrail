#include "ui/Delegates.h"

#include "ui/CoverCache.h"
#include "ui/Labels.h"
#include "ui/PublicationListModel.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

#include <algorithm>

namespace papertrail::ui {

namespace {

const int kCoverWidth = CoverCache::cardSize().width();
const int kCoverHeight = CoverCache::cardSize().height();
constexpr int kCardPadding = 8;

// All colours come from the palette, so that light and dark themes both work.
QColor faint(const QPalette &palette)
{
    QColor colour = palette.color(QPalette::Text);
    colour.setAlphaF(0.12f);
    return colour;
}

QColor muted(const QPalette &palette, bool selected)
{
    QColor colour = palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    colour.setAlphaF(0.65f);
    return colour;
}

// Draws text across up to `maxLines` lines, ending in an ellipsis if there's more. Returns the
// height used. This never interprets the text, which may have come from a document's metadata.
int drawLines(QPainter *painter, const QRect &rect, const QString &text, int maxLines)
{
    const QFontMetrics metrics(painter->font());
    QTextLayout layout(text, painter->font());
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(textOption);
    layout.beginLayout();
    int y = 0;
    for (int lineNumber = 0; lineNumber < maxLines; ++lineNumber) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(rect.width());
        const QPoint position(rect.left(), rect.top() + y + metrics.ascent());
        if (lineNumber == maxLines - 1 && line.textStart() + line.textLength() < text.size()) {
            const QString rest = text.mid(line.textStart()).simplified();
            painter->drawText(position, metrics.elidedText(rest, Qt::ElideRight, rect.width()));
        } else {
            painter->drawText(position, text.mid(line.textStart(), line.textLength()).trimmed());
        }
        y += metrics.lineSpacing();
    }
    layout.endLayout();
    return y;
}

void drawCover(QPainter *painter, const QRect &area, const QPixmap &cover, const QString &format,
               const QPalette &palette)
{
    if (!cover.isNull()) {
        const QSize size = cover.size().scaled(area.size(), Qt::KeepAspectRatio);
        const QRect target(area.left() + (area.width() - size.width()) / 2,
                           area.top() + (area.height() - size.height()) / 2, size.width(), size.height());
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawPixmap(target, cover);
        painter->setPen(faint(palette));
        painter->drawRect(target.adjusted(0, 0, -1, -1));
        return;
    }

    painter->fillRect(area, faint(palette));
    if (!format.isEmpty()) {
        painter->setPen(muted(palette, false));
        QFont small = painter->font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        painter->save();
        painter->setFont(small);
        painter->drawText(area, Qt::AlignCenter, format.toUpper());
        painter->restore();
    }
}

void drawNotices(QPainter *painter, const QRect &cover, const QModelIndex &index, const QPalette &palette, int dot)
{
    // A retraction goes along the very bottom, and a problem with the file just above it.
    const int bar = drawNoticeBar(painter, cover, coverNoticeLabel(index.data(PublicationListModel::CoverNoticeRole).toString()));
    const QString problem = fileStatusLabel(index.data(PublicationListModel::FileStatusRole).toString());
    if (!problem.isEmpty() && cover.height() > 60) {
        const QRect strip(cover.left(), cover.bottom() - bar - 22, cover.width(), 23);
        painter->fillRect(strip, palette.color(QPalette::Window));
        painter->setPen(palette.color(QPalette::WindowText));
        painter->drawText(strip, Qt::AlignCenter, problem);
    }
    if (index.data(PublicationListModel::NeedsReviewRole).toBool()) {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(palette.color(QPalette::Window));
        painter->setBrush(QColor(0xE0, 0x8A, 0x00));
        painter->drawEllipse(QRect(cover.right() - dot - 3, cover.top() + 3, dot, dot));
        painter->setBrush(Qt::NoBrush);
        painter->setRenderHint(QPainter::Antialiasing, false);
    }
}

// Wraps text at `width`, keeping its own line breaks, and returns the height it takes. Draws it
// with its top at `top` too, when given a painter. Never interprets the text.
int wrappedText(QPainter *painter, const QFont &font, int left, int top, int width, const QString &text)
{
    const QFontMetrics metrics(font);
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    int y = 0;
    const QStringList paragraphs = QString(text).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).split(u'\n');
    for (const QString &paragraph : paragraphs) {
        QTextLayout layout(paragraph, font);
        layout.setTextOption(textOption);
        layout.beginLayout();
        bool any = false;
        for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(std::max(width, 1));
            if (painter)
                painter->drawText(QPoint(left, top + y + metrics.ascent()), paragraph.mid(line.textStart(), line.textLength()).trimmed());
            y += metrics.lineSpacing();
            any = true;
        }
        layout.endLayout();
        if (!any)
            y += metrics.lineSpacing(); // A blank line in a note.
    }
    return y;
}

constexpr int kNotePadding = 6;  // Around each entry.
constexpr int kNoteGap = 4;      // Between its parts.
constexpr int kQuoteBar = 3;     // The bar in the highlight's colour.
constexpr int kQuoteIndent = 8;  // From the bar's left to the highlighted words.
constexpr int kNoteBoxPadding = 4;

// The colour a style itself would give an item's text. The Windows 10 and 11 styles draw a pale
// selection and keep the ordinary text colour on it, where others fill it with the highlight colour.
QColor itemTextColour(const QStyleOptionViewItem &option)
{
    const QPalette::ColorGroup group = !(option.state & QStyle::State_Enabled) ? QPalette::Disabled
                                       : (option.state & QStyle::State_Active) ? QPalette::Active
                                                                                : QPalette::Inactive;
    const QString style = (option.widget ? option.widget->style() : QApplication::style())->name();
    const bool paleSelection = style.compare(QStringLiteral("windowsvista"), Qt::CaseInsensitive) == 0
                               || style.compare(QStringLiteral("windows11"), Qt::CaseInsensitive) == 0;
    const bool selected = option.state & QStyle::State_Selected;
    return option.palette.color(group, selected && !paleSelection ? QPalette::HighlightedText : QPalette::Text);
}

// Lays out one entry of the Notes panel, drawing it when given a painter, and returns its height.
int layOutAnnotation(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index, const QRect &area)
{
    const QString heading = index.data(AnnotationDelegate::HeadingRole).toString();
    const QString quote = index.data(AnnotationDelegate::QuoteRole).toString();
    const QString note = index.data(AnnotationDelegate::NoteRole).toString();
    const QColor textColour = itemTextColour(option);
    QColor dimmed = textColour;
    dimmed.setAlphaF(0.65f);

    QFont small = option.font;
    small.setPointSizeF(small.pointSizeF() * 0.85);
    int y = area.top();
    bool first = true;
    const auto startPart = [&] {
        if (!first)
            y += kNoteGap;
        first = false;
    };

    if (!heading.isEmpty()) {
        startPart();
        if (painter) {
            painter->setFont(small);
            painter->setPen(dimmed);
        }
        y += wrappedText(painter, small, area.left(), y, area.width(), heading);
    }
    if (!quote.isEmpty()) {
        startPart();
        const int textLeft = area.left() + kQuoteIndent;
        if (painter) {
            painter->setFont(option.font);
            painter->setPen(dimmed);
        }
        const int height = wrappedText(painter, option.font, textLeft, y, area.right() - textLeft, quote);
        if (painter) {
            QColor colour(index.data(AnnotationDelegate::ColourRole).toString());
            if (!colour.isValid())
                colour = faint(option.palette);
            painter->fillRect(QRect(area.left(), y, kQuoteBar, height), colour);
        }
        y += height;
    }
    if (!note.isEmpty()) {
        startPart();
        const int innerWidth = area.width() - 2 * kNoteBoxPadding;
        if (painter) {
            painter->setFont(option.font);
            const int height = wrappedText(nullptr, option.font, 0, 0, innerWidth, note);
            QColor box = textColour;
            box.setAlphaF(0.08f);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(box);
            painter->drawRoundedRect(QRect(area.left(), y, area.width(), height + 2 * kNoteBoxPadding), 3, 3);
            painter->setBrush(Qt::NoBrush);
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setPen(textColour);
        }
        y += 2 * kNoteBoxPadding
             + wrappedText(painter, option.font, area.left() + kNoteBoxPadding, y + kNoteBoxPadding, innerWidth, note);
    }
    return y - area.top();
}

// Lays out one article of a list, drawing it when given a painter, and returns its height.
int layOutArticle(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index, const QRect &area)
{
    const QString title = index.data(ArticleDelegate::TitleRole).toString();
    const QString pages = index.data(ArticleDelegate::PagesRole).toString();
    QColor dimmed = itemTextColour(option);
    dimmed.setAlphaF(0.65f);
    QFont small = option.font;
    small.setPointSizeF(small.pointSizeF() * 0.85);

    if (painter) {
        painter->setFont(option.font);
        painter->setPen(itemTextColour(option));
    }
    int height = wrappedText(painter, option.font, area.left(), area.top(), area.width(), title);
    if (!pages.isEmpty()) {
        height += 2;
        if (painter) {
            painter->setFont(small);
            painter->setPen(dimmed);
        }
        height += wrappedText(painter, small, area.left(), area.top() + height, area.width(), pages);
    }
    return height;
}

// The style's background, selection and focus for an item, without its text or icon, which the
// delegates draw themselves.
void drawItemBackground(QPainter *painter, QStyleOptionViewItem opt)
{
    opt.text.clear();
    opt.icon = QIcon();
    opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
    const QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
}

// A rule under each entry of a list, so where one ends is plain.
void drawRuleBelow(QPainter *painter, const QStyleOptionViewItem &option)
{
    painter->setPen(faint(option.palette));
    painter->drawLine(option.rect.left(), option.rect.bottom(), option.rect.right(), option.rect.bottom());
}

// The width a list's entries wrap to, which the option's rect doesn't reliably give in sizeHint.
int listWidth(const QStyleOptionViewItem &option)
{
    if (const auto *view = qobject_cast<const QAbstractScrollArea *>(option.widget))
        return view->viewport()->width();
    return option.rect.width();
}

} // namespace

int drawNoticeBar(QPainter *painter, const QRect &cover, const QString &label)
{
    if (label.isEmpty() || cover.isEmpty())
        return 0;
    painter->save();
    QFont font = painter->font();
    font.setBold(true);
    const QFontMetrics metrics(font);
    const bool words = metrics.horizontalAdvance(label) + 6 <= cover.width() && cover.height() > 3 * metrics.height();
    const int height = words ? metrics.height() + 6 : qMax(4, cover.height() / 6);
    const QRect bar(cover.left(), cover.bottom() + 1 - height, cover.width(), height);
    painter->fillRect(bar, QColor(0xc6, 0x28, 0x28));
    if (words) {
        painter->setFont(font);
        painter->setPen(Qt::white);
        painter->drawText(bar, Qt::AlignCenter, label);
    }
    painter->restore();
    return height;
}

GridDelegate::GridDelegate(CoverCache &covers, QObject *parent)
    : QStyledItemDelegate(parent), m_covers(covers)
{
}

QSize GridDelegate::cardSize()
{
    const QFontMetrics metrics(QApplication::font());
    return {kCoverWidth + 2 * kCardPadding, kCoverHeight + 3 * kCardPadding + 4 * metrics.lineSpacing() + 2};
}

QSize GridDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return cardSize();
}

void GridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    const QRect card = option.rect.adjusted(2, 2, -2, -2);
    const bool selected = option.state & QStyle::State_Selected;
    if (selected) {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.palette.color(QPalette::Highlight));
        painter->drawRoundedRect(card, 4, 4);
        painter->setBrush(Qt::NoBrush);
        painter->setRenderHint(QPainter::Antialiasing, false);
    }

    const int coverLeft = card.left() + (card.width() - kCoverWidth) / 2;
    const QRect coverArea(coverLeft, card.top() + kCardPadding - 2, kCoverWidth, kCoverHeight);
    const QRect textArea(coverLeft, coverArea.bottom() + kCardPadding, kCoverWidth,
                         card.bottom() - coverArea.bottom() - kCardPadding);

    if (!index.data(PublicationListModel::IsLoadedRole).toBool()) {
        // Waiting for this row to be fetched.
        painter->fillRect(coverArea, faint(option.palette));
        const int line = QFontMetrics(painter->font()).height() - 4;
        painter->fillRect(QRect(textArea.left(), textArea.top(), textArea.width(), line), faint(option.palette));
        painter->fillRect(QRect(textArea.left(), textArea.top() + line + 6, textArea.width() * 2 / 3, line),
                          faint(option.palette));
        painter->restore();
        return;
    }

    drawCover(painter, coverArea, m_covers.cover(index.data(PublicationListModel::CoverShaRole).toString()),
              index.data(PublicationListModel::FormatRole).toString(), option.palette);
    drawNotices(painter, coverArea, index, option.palette, 12);

    QRect rest = textArea;
    QFont titleFont = painter->font();
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
    rest.setTop(rest.top() + drawLines(painter, rest, index.data(PublicationListModel::TitleRole).toString(), 2));

    titleFont.setBold(false);
    painter->setFont(titleFont);
    painter->setPen(muted(option.palette, selected));
    rest.setTop(rest.top() + drawLines(painter, rest, index.data(PublicationListModel::AuthorsRole).toString(), 1));
    // The formats only earn their space on the card when there's a choice of them.
    QStringList lastLine;
    const QString numbering = index.data(PublicationListModel::NumberingRole).toString();
    const QVariant year = index.data(PublicationListModel::YearRole);
    if (!numbering.isEmpty())
        lastLine << numbering; // An issue's date is part of its numbering.
    else if (year.isValid())
        lastLine << year.toString();
    const QString formats = index.data(PublicationListModel::FormatsRole).toString();
    if (formats.contains(u','))
        lastLine << formats;
    if (!lastLine.isEmpty())
        drawLines(painter, rest, lastLine.join(QStringLiteral(" \u00B7 ")), 1);

    if (option.state & QStyle::State_HasFocus) {
        painter->setPen(QPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Highlight), 1,
                             Qt::DotLine));
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), 3, 3);
    }
    painter->restore();
}

ListTitleDelegate::ListTitleDelegate(CoverCache &covers, QObject *parent)
    : QStyledItemDelegate(parent), m_covers(covers)
{
}

int ListTitleDelegate::rowHeight()
{
    return 46;
}

void ListTitleDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString title = opt.text;
    opt.text.clear();
    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const QRect coverArea(option.rect.left() + 4, option.rect.top() + 3, 30, 40);
    const QRect textArea = option.rect.adjusted(coverArea.width() + 12, 2, -4, -2);
    const bool selected = option.state & QStyle::State_Selected;

    if (!index.data(PublicationListModel::IsLoadedRole).toBool()) {
        painter->fillRect(coverArea, faint(option.palette));
        painter->restore();
        return;
    }

    drawCover(painter, coverArea, m_covers.cover(index.data(PublicationListModel::CoverShaRole).toString()), QString(),
              option.palette);
    drawNotices(painter, coverArea, index, option.palette, 8);

    painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
    const QFontMetrics metrics(painter->font());
    const bool fitsOnOneLine = metrics.horizontalAdvance(title) <= textArea.width();
    const int lines = !fitsOnOneLine && metrics.lineSpacing() * 2 <= textArea.height() ? 2 : 1;
    const int used = lines * metrics.lineSpacing();
    drawLines(painter, QRect(textArea.left(), textArea.top() + (textArea.height() - used) / 2, textArea.width(), used),
              title, lines);

    const QString problem = fileStatusLabel(index.data(PublicationListModel::FileStatusRole).toString());
    if (!problem.isEmpty()) {
        const int width = metrics.horizontalAdvance(problem) + 10;
        const QRect badge(textArea.right() - width, option.rect.top() + 3, width, metrics.height());
        if (textArea.width() > 2 * width) {
            painter->fillRect(badge, option.palette.color(QPalette::Window));
            painter->setPen(option.palette.color(QPalette::WindowText));
            painter->drawText(badge, Qt::AlignCenter, problem);
        }
    }
    painter->restore();
}

void AnnotationDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    drawItemBackground(painter, opt);
    layOutAnnotation(painter, opt, index, option.rect.adjusted(kNotePadding, kNotePadding, -kNotePadding, -kNotePadding));
    drawRuleBelow(painter, option);
    painter->restore();
}

QSize AnnotationDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int width = listWidth(option);
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QRect area(0, 0, std::max(width - 2 * kNotePadding, 40), 0);
    return {width, layOutAnnotation(nullptr, opt, index, area) + 2 * kNotePadding + 1};
}

void ArticleDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    drawItemBackground(painter, opt);
    layOutArticle(painter, opt, index, option.rect.adjusted(kNotePadding, kNotePadding, -kNotePadding, -kNotePadding));
    drawRuleBelow(painter, option);
    painter->restore();
}

QSize ArticleDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int width = listWidth(option);
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QRect area(0, 0, std::max(width - 2 * kNotePadding, 40), 0);
    return {width, layOutArticle(nullptr, opt, index, area) + 2 * kNotePadding + 1};
}

} // namespace papertrail::ui
