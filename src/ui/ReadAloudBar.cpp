#include "ui/ReadAloudBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

namespace papertrail::ui {

namespace {

constexpr int kSettleMs = 300;

} // namespace

ReadAloudBar::ReadAloudBar(QWidget *parent)
    : QWidget(parent)
{
    setAccessibleName(tr("Reading aloud"));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(8, 4, 8, 4);

    const auto button = [this, row](QStyle::StandardPixmap icon, const QString &tip, const char *name) {
        auto *tool = new QToolButton(this);
        tool->setIcon(style()->standardIcon(icon));
        tool->setToolTip(tip);
        tool->setAccessibleName(tip);
        tool->setObjectName(QString::fromLatin1(name));
        tool->setAutoRaise(true);
        row->addWidget(tool);
        return tool;
    };
    m_previous = button(QStyle::SP_MediaSkipBackward, tr("Previous sentence"), "readPrevious");
    m_playPause = button(QStyle::SP_MediaPause, tr("Pause"), "readPlayPause");
    m_next = button(QStyle::SP_MediaSkipForward, tr("Next sentence"), "readNext");
    m_stop = button(QStyle::SP_MediaStop, tr("Stop reading aloud"), "readStop");
    connect(m_previous, &QToolButton::clicked, this, &ReadAloudBar::previousRequested);
    connect(m_playPause, &QToolButton::clicked, this, &ReadAloudBar::playPauseRequested);
    connect(m_next, &QToolButton::clicked, this, &ReadAloudBar::nextRequested);
    connect(m_stop, &QToolButton::clicked, this, &ReadAloudBar::stopRequested);

    row->addSpacing(12);
    m_where = new QLabel(this);
    m_where->setTextFormat(Qt::PlainText);
    row->addWidget(m_where, 1);

    const auto slider = [this, row](const QString &label, int lowest, int highest, const QString &tip) {
        auto *text = new QLabel(label, this);
        row->addWidget(text);
        auto *made = new QSlider(Qt::Horizontal, this);
        made->setRange(lowest, highest);
        made->setFixedWidth(110);
        made->setToolTip(tip);
        made->setAccessibleName(label);
        text->setBuddy(made);
        row->addWidget(made);
        return made;
    };
    m_rate = slider(tr("&Speed"), -10, 10, tr("Slower to the left, faster to the right"));
    m_volume = slider(tr("&Volume"), 0, 10, tr("Quieter to the left, louder to the right"));

    m_rateSettled = new QTimer(this);
    m_rateSettled->setSingleShot(true);
    m_rateSettled->setInterval(kSettleMs);
    connect(m_rate, &QSlider::valueChanged, m_rateSettled, qOverload<>(&QTimer::start));
    connect(m_rateSettled, &QTimer::timeout, this, [this] { emit rateChosen(m_rate->value() / 10.0); });
    m_volumeSettled = new QTimer(this);
    m_volumeSettled->setSingleShot(true);
    m_volumeSettled->setInterval(kSettleMs);
    connect(m_volume, &QSlider::valueChanged, m_volumeSettled, qOverload<>(&QTimer::start));
    connect(m_volumeSettled, &QTimer::timeout, this, [this] { emit volumeChosen(m_volume->value() / 10.0); });
}

void ReadAloudBar::setPaused(bool paused)
{
    m_playPause->setIcon(style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
    const QString tip = paused ? tr("Carry on") : tr("Pause");
    m_playPause->setToolTip(tip);
    m_playPause->setAccessibleName(tip);
}

void ReadAloudBar::setWhere(const QString &text)
{
    m_where->setText(text);
}

void ReadAloudBar::setRate(double rate)
{
    const QSignalBlocker quiet(m_rate);
    m_rate->setValue(qRound(qBound(-1.0, rate, 1.0) * 10));
}

void ReadAloudBar::setVolume(double volume)
{
    const QSignalBlocker quiet(m_volume);
    m_volume->setValue(qRound(qBound(0.0, volume, 1.0) * 10));
}

} // namespace papertrail::ui
