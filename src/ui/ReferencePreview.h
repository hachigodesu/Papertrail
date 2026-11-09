#pragma once

#include <QFrame>
#include <QImage>
#include <QString>

class QLabel;
class QPushButton;

namespace papertrail::ui {

// A look at what a reference points at, such as a figure and its caption or an entry in the references, without
// leaving the place being read. Clicking it, Go there or Enter goes there; Escape or a click elsewhere closes it.
class ReferencePreview : public QFrame {
    Q_OBJECT

public:
    // `heading` comes from the document's text, so it's shown as plain text.
    ReferencePreview(const QString &heading, const QImage &picture, QWidget *parent = nullptr);

    // Shows it beside a point in `view`, kept on the screen.
    void popUpNear(QWidget *view, const QPoint &point);

    QLabel *headingLabel() const { return m_heading; }
    QLabel *pictureLabel() const { return m_picture; }
    QPushButton *goButton() const { return m_go; }

signals:
    void goRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void go();

    QLabel *m_heading = nullptr;
    QLabel *m_picture = nullptr;
    QPushButton *m_go = nullptr;
};

// A rendered page's colours as the reader shows them: darkened for dark pages, unless `keepPictures` because it's a
// picture, and warmed for sepia. `colours` is PageView::Colours as an int.
QImage withPageColours(const QImage &image, int colours, bool keepPictures);

} // namespace papertrail::ui
