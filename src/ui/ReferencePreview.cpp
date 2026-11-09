#include "ui/ReferencePreview.h"

#include "ui/PageView.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace papertrail::ui {

QImage withPageColours(const QImage &image, int colours, bool keepPictures)
{
    const auto chosen = PageView::Colours(colours);
    if (image.isNull() || chosen == PageView::Colours::Light || (chosen == PageView::Colours::Dark && keepPictures))
        return image;
    QImage coloured = image.convertToFormat(QImage::Format_RGB32);
    QPainter painter(&coloured);
    // The same as the page view does as it paints.
    if (chosen == PageView::Colours::Dark) {
        painter.setCompositionMode(QPainter::CompositionMode_Difference);
        painter.fillRect(coloured.rect(), QColor(232, 232, 232));
    } else {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.fillRect(coloured.rect(), QColor(244, 233, 208));
    }
    painter.end();
    coloured.setDevicePixelRatio(image.devicePixelRatio());
    return coloured;
}

ReferencePreview::ReferencePreview(const QString &heading, const QImage &picture, QWidget *parent)
    : QFrame(parent, Qt::Popup)
{
    setObjectName(QStringLiteral("referencePreview"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(6);

    auto *top = new QHBoxLayout;
    m_heading = new QLabel(heading, this);
    m_heading->setTextFormat(Qt::PlainText);
    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    top->addWidget(m_heading, 1);
    m_go = new QPushButton(tr("Go there"), this);
    m_go->setObjectName(QStringLiteral("goThere"));
    m_go->setDefault(true);
    connect(m_go, &QPushButton::clicked, this, &ReferencePreview::go);
    top->addWidget(m_go);
    layout->addLayout(top);

    m_picture = new QLabel(this);
    m_picture->setPixmap(QPixmap::fromImage(picture));
    m_picture->setCursor(Qt::PointingHandCursor);
    m_picture->setToolTip(tr("Go there"));
    m_picture->installEventFilter(this);
    layout->addWidget(m_picture);
}

void ReferencePreview::go()
{
    close();
    emit goRequested();
}

void ReferencePreview::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        go();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QFrame::keyPressEvent(event);
}

bool ReferencePreview::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_picture && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        go();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void ReferencePreview::popUpNear(QWidget *view, const QPoint &point)
{
    adjustSize();
    // Below the click, or above it if there's no room, and never off the screen.
    QPoint place = view->mapToGlobal(point + QPoint(-width() / 3, 18));
    if (const QScreen *screen = view->screen()) {
        const QRect available = screen->availableGeometry();
        if (place.y() + height() > available.bottom())
            place.setY(view->mapToGlobal(point).y() - height() - 12);
        place.setX(qBound(available.left(), place.x(), qMax(available.left(), available.right() - width())));
        place.setY(qBound(available.top(), place.y(), qMax(available.top(), available.bottom() - height())));
    }
    move(place);
    show();
    setFocus();
}

} // namespace papertrail::ui
