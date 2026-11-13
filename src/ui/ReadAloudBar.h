#pragma once

#include <QWidget>

class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace papertrail::ui {

// The controls for reading aloud, along the bottom of a document while it reads: back and on a sentence, pause and
// carry on, stop, the speed and the volume, and where it's got to.
class ReadAloudBar : public QWidget {
    Q_OBJECT

public:
    explicit ReadAloudBar(QWidget *parent = nullptr);

    void setPaused(bool paused);
    void setWhere(const QString &text);
    // From -1, slowest, to 1, fastest, and from 0 to 1, without saying they've been chosen.
    void setRate(double rate);
    void setVolume(double volume);

    // For tests.
    QToolButton *previousButton() const { return m_previous; }
    QToolButton *playPauseButton() const { return m_playPause; }
    QToolButton *nextButton() const { return m_next; }
    QToolButton *stopButton() const { return m_stop; }
    QSlider *rateSlider() const { return m_rate; }
    QSlider *volumeSlider() const { return m_volume; }
    QLabel *whereLabel() const { return m_where; }

signals:
    void previousRequested();
    void playPauseRequested();
    void nextRequested();
    void stopRequested();
    // Once a slider has been still for a moment, so dragging it doesn't start the sentence again and again.
    void rateChosen(double rate);
    void volumeChosen(double volume);

private:
    QToolButton *m_previous = nullptr;
    QToolButton *m_playPause = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_stop = nullptr;
    QSlider *m_rate = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_where = nullptr;
    QTimer *m_rateSettled = nullptr;
    QTimer *m_volumeSettled = nullptr;
};

} // namespace papertrail::ui
