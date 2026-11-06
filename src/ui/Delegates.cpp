#include "ui/Delegates.h"

#include "ui/CoverCache.h"
#include "ui/Labels.h"
#include "ui/PublicationListModel.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

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
    const QString problem = fileStatusLabel(index.data(PublicationListModel::FileStatusRole).toString());
    if (!problem.isEmpty() && cover.height() > 60) {
        const QRect strip(cover.left(), cover.bottom() - 22, cover.width(), 23);
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

} // namespace

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
    const QVariant year = index.data(PublicationListModel::YearRole);
    if (year.isValid())
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

} // namespace papertrail::ui
