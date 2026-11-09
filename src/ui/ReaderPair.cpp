#include "ui/ReaderPair.h"

#include "ui/ReaderTab.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QSplitter>

namespace papertrail::ui {

namespace {

// Each side's pages are held in memory near what's shown, so two together get half as much each.
constexpr qint64 kSideImageBytes = 200 * 1024 * 1024;
constexpr qint64 kWholeImageBytes = 400 * 1024 * 1024;

bool sameDocument(const ReaderTab *a, const ReaderTab *b)
{
    return a && b && a->publicationId() == b->publicationId() && a->fileId() == b->fileId();
}

} // namespace

ReaderPair::ReaderPair(ReaderTab *left, ReaderTab *right, QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("readerPair"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter);
    m_left = left;
    m_right = right;
    adopt(left, 0);
    adopt(right, 1);
    m_splitter->setSizes({1, 1});
    m_active = left;

    // The side last clicked or typed in is the one being read.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (ReaderTab *reader : readers()) {
            if (now && (now == reader || reader->isAncestorOf(now))) {
                setActive(reader);
                return;
            }
        }
    });
    updatePlaces();
}

QList<ReaderTab *> ReaderPair::readers() const
{
    QList<ReaderTab *> both;
    if (m_left)
        both << m_left;
    if (m_right)
        both << m_right;
    return both;
}

ReaderTab *ReaderPair::activeReader() const
{
    if (m_active)
        return m_active;
    return m_left ? m_left : m_right;
}

ReaderTab *ReaderPair::otherThan(const ReaderTab *reader) const
{
    return reader == m_left ? m_right : m_left;
}

void ReaderPair::setActive(ReaderTab *reader)
{
    if (!reader || reader == m_active || (reader != m_left && reader != m_right))
        return;
    m_active = reader;
    updatePlaces();
    emit activeChanged(reader);
}

void ReaderPair::adopt(ReaderTab *reader, int index)
{
    m_splitter->insertWidget(index, reader);
    reader->setInPair(true);
    reader->setImageBudget(kSideImageBytes);
    reader->show();
}

void ReaderPair::release(ReaderTab *reader)
{
    reader->setInPair(false);
    reader->setImageBudget(kWholeImageBytes);
    reader->setSavesPlace(true);
    reader->setParent(nullptr);
}

ReaderTab *ReaderPair::take(ReaderTab *reader)
{
    if (reader != m_left && reader != m_right)
        return nullptr;
    // What it shows now is the place to keep.
    reader->setSavesPlace(true);
    reader->saveState();
    if (reader == m_left)
        m_left = nullptr;
    else
        m_right = nullptr;
    if (m_active == reader)
        m_active = m_left ? m_left : m_right;
    release(reader);
    updatePlaces();
    return reader;
}

ReaderTab *ReaderPair::replace(ReaderTab *side, ReaderTab *reader)
{
    if (side != m_left && side != m_right)
        return nullptr;
    const int index = m_splitter->indexOf(side);
    const QList<int> sizes = m_splitter->sizes();
    side->setSavesPlace(true);
    side->saveState();
    if (side == m_left)
        m_left = reader;
    else
        m_right = reader;
    release(side);
    adopt(reader, index);
    m_splitter->setSizes(sizes);
    m_active = reader;
    updatePlaces();
    emit activeChanged(reader);
    return side;
}

// Places are kept by document, so where both sides show the same one only the side being read keeps its place.
void ReaderPair::updatePlaces()
{
    const bool same = sameDocument(m_left, m_right);
    for (ReaderTab *reader : readers())
        reader->setSavesPlace(!same || reader == activeReader());
}

} // namespace papertrail::ui
