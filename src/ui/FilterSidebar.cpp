#include "ui/FilterSidebar.h"

#include "core/repo/CollectionRepository.h"
#include "core/repo/TagRepository.h"
#include "ui/GridView.h"
#include "ui/Labels.h"

#include <QColorDialog>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace core;

namespace {

constexpr int kKeyRole = Qt::UserRole + 1;
constexpr int kSectionRole = Qt::UserRole + 2;
constexpr int kFacetLimit = 50;
constexpr int kNoSection = -1;
constexpr int kSavedSearch = -2; // An entry under Saved searches, which isn't a filter.

const QLatin1StringView kFlagReview("review");
const QLatin1StringView kFlagNoText("no_text");
const QLatin1StringView kFlagNoFile("no_file");
const QLatin1StringView kFlagMissing("missing");
const QLatin1StringView kFlagUnreadable("unreadable");
const QLatin1StringView kFlagRetracted("retracted");

Facet facetFor(FilterSidebar::Section section)
{
    switch (section) {
    case FilterSidebar::Authors: return Facet::Author;
    case FilterSidebar::Journals: return Facet::Journal;
    case FilterSidebar::Publishers: return Facet::Publisher;
    case FilterSidebar::Conferences: return Facet::Conference;
    case FilterSidebar::Languages: return Facet::Language;
    case FilterSidebar::Formats: return Facet::Format;
    case FilterSidebar::Courts: return Facet::Court;
    case FilterSidebar::Jurisdictions: return Facet::Jurisdiction;
    default: return Facet::ReadStatus;
    }
}

QString facetLabel(FilterSidebar::Section section, const FacetValue &value)
{
    if (section == FilterSidebar::ReadStatuses)
        return readStatusLabelFromDb(value.key);
    if (section == FilterSidebar::Formats)
        return value.label.toUpper();
    return value.label;
}

// Some styles draw item checkboxes and arrows that can barely be seen in dark mode, so these are
// drawn here using the palette's own text colour.
class VisibleChecksStyle : public QProxyStyle {
public:
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override
    {
        if (element == PE_IndicatorBranch) {
            if (!(option->state & State_Children))
                return;
            QColor colour = option->palette.color(QPalette::Text);
            colour.setAlphaF(0.75f);
            const QPointF c = QRectF(option->rect).center();
            const bool open = option->state & State_Open;
            const QPolygonF arrow = open ? QPolygonF({c + QPointF(-4, -2), c + QPointF(4, -2), c + QPointF(0, 3)})
                                         : QPolygonF({c + QPointF(-2, -4), c + QPointF(3, 0), c + QPointF(-2, 4)});
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(colour);
            painter->drawPolygon(arrow);
            painter->restore();
            return;
        }
        if (element != PE_IndicatorItemViewItemCheck) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const int side = qMin(option->rect.width(), option->rect.height()) - 2;
        const QRectF box(option->rect.center().x() - side / 2.0 + 0.5, option->rect.center().y() - side / 2.0 + 0.5, side, side);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (option->state & State_On) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(option->palette.color(QPalette::Highlight));
            painter->drawRoundedRect(box, 2, 2);
            QPainterPath tick;
            tick.moveTo(box.left() + side * 0.22, box.top() + side * 0.52);
            tick.lineTo(box.left() + side * 0.42, box.top() + side * 0.72);
            tick.lineTo(box.left() + side * 0.78, box.top() + side * 0.30);
            painter->setPen(QPen(option->palette.color(QPalette::HighlightedText), 1.6));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(tick);
        } else {
            QColor edge = option->palette.color(QPalette::Text);
            edge.setAlphaF(0.6f);
            painter->setPen(QPen(edge, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(box, 2, 2);
        }
        painter->restore();
    }
};

QIcon colourSwatch(const QString &colour)
{
    const QColor parsed(colour);
    if (colour.isEmpty() || !parsed.isValid())
        return {};
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(parsed);
    painter.drawEllipse(pixmap.rect());
    return QIcon(pixmap);
}

} // namespace

FilterSidebar::FilterSidebar(TagRepository &tags, CollectionRepository &collections, QWidget *parent)
    : QWidget(parent), m_tags(tags), m_collections(collections)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(tr("Filters"), this);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    m_clear = new QPushButton(tr("Clear"), this);
    m_clear->setEnabled(false);
    connect(m_clear, &QPushButton::clicked, this, &FilterSidebar::clearSelection);
    heading->addWidget(title);
    heading->addStretch();
    heading->addWidget(m_clear);
    layout->addLayout(heading);

    auto *years = new QHBoxLayout;
    years->addWidget(new QLabel(tr("Year"), this));
    m_yearFrom = new QLineEdit(this);
    m_yearTo = new QLineEdit(this);
    m_yearFrom->setAccessibleName(tr("From year"));
    m_yearTo->setAccessibleName(tr("To year"));
    for (QLineEdit *edit : {m_yearFrom, m_yearTo}) {
        edit->setValidator(new QIntValidator(0, 9999, edit));
        edit->setClearButtonEnabled(false);
        edit->setMaximumWidth(70);
        connect(edit, &QLineEdit::editingFinished, this, [this] {
            if (m_yearFrom->isModified() || m_yearTo->isModified()) {
                m_yearFrom->setModified(false);
                m_yearTo->setModified(false);
                updateClearButton();
                emit filterChanged();
            }
        });
    }
    years->addWidget(m_yearFrom);
    years->addWidget(new QLabel(tr("to"), this));
    years->addWidget(m_yearTo);
    years->addStretch();
    layout->addLayout(years);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // Publications dragged from the library can be dropped on a collection or tag.
    m_tree->viewport()->setAcceptDrops(true);
    m_treeViewport = m_tree->viewport();
    m_treeViewport->installEventFilter(this);
    m_tree->setAccessibleName(tr("Filters"));
    auto *checks = new VisibleChecksStyle;
    checks->setParent(m_tree);
    m_tree->setStyle(checks);
    layout->addWidget(m_tree, 1);

    buildSections();
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) { onItemChanged(item); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &FilterSidebar::showContextMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, kSectionRole).toInt() == kSavedSearch)
            emit savedSearchChosen(item->data(0, kKeyRole).toLongLong());
    });
}

SidebarChoices FilterSidebar::choices() const
{
    SidebarChoices choices;
    for (int section = 0; section < SectionCount; ++section) {
        if (!isShown(section))
            continue;
        QStringList keys(m_chosen[section].cbegin(), m_chosen[section].cend());
        keys.sort();
        for (const QString &key : keys)
            choices.values[section] << qMakePair(key, m_chosenLabels[section].value(key, key));
    }
    choices.yearFrom = m_yearFrom->text();
    choices.yearTo = m_yearTo->text();
    return choices;
}

void FilterSidebar::setChoices(const SidebarChoices &choices)
{
    {
        const QSignalBlocker blocker(m_tree);
        for (int section = 0; section < SectionCount; ++section) {
            m_chosen[section].clear();
            m_chosenLabels[section].clear();
            for (const auto &[key, label] : choices.values.value(section)) {
                m_chosen[section].insert(key);
                m_chosenLabels[section].insert(key, label);
            }
        }
        QTreeWidgetItemIterator it(m_tree);
        while (*it) {
            const int section = (*it)->data(0, kSectionRole).toInt();
            if (((*it)->flags() & Qt::ItemIsUserCheckable) && section >= 0 && section < SectionCount) {
                const bool chosen = m_chosen[section].contains((*it)->data(0, kKeyRole).toString());
                (*it)->setCheckState(0, chosen ? Qt::Checked : Qt::Unchecked);
            }
            ++it;
        }
    }
    m_yearFrom->setText(choices.yearFrom);
    m_yearTo->setText(choices.yearTo);
    updateClearButton();
    emit filterChanged();
}

void FilterSidebar::setSavedSearches(const QList<QPair<qint64, QString>> &searches)
{
    qDeleteAll(m_savedSearches->takeChildren());
    for (const auto &[id, name] : searches) {
        // Shown as it is, since a name could be anything.
        auto *item = new QTreeWidgetItem(m_savedSearches, {name});
        item->setFlags(Qt::ItemIsEnabled);
        item->setToolTip(0, tr("Shows this search. Right-click to change it."));
        item->setData(0, kKeyRole, id);
        item->setData(0, kSectionRole, kSavedSearch);
    }
    if (searches.isEmpty()) {
        auto *note = new QTreeWidgetItem(m_savedSearches, {tr("Right-click to save the current search.")});
        note->setFlags(Qt::ItemIsEnabled);
        note->setData(0, kSectionRole, kNoSection);
        note->setForeground(0, palette().color(QPalette::PlaceholderText));
    }
    m_savedSearches->setExpanded(true);
}

void FilterSidebar::buildSections()
{
    const auto add = [this](Section section, const QString &name) {
        auto *item = new QTreeWidgetItem(m_tree, {name});
        item->setFlags(Qt::ItemIsEnabled);
        item->setData(0, kSectionRole, kNoSection);
        QFont bold = item->font(0);
        bold.setBold(true);
        item->setFont(0, bold);
        m_sections[section] = item;
    };
    add(Collections, tr("Collections"));
    m_savedSearches = new QTreeWidgetItem(m_tree, {tr("Saved searches")});
    m_savedSearches->setFlags(Qt::ItemIsEnabled);
    m_savedSearches->setData(0, kSectionRole, kNoSection);
    QFont savedFont = m_savedSearches->font(0);
    savedFont.setBold(true);
    m_savedSearches->setFont(0, savedFont);
    setSavedSearches({});
    add(Tags, tr("Tags"));
    add(Status, tr("Needs attention"));
    add(Authors, tr("Author"));
    add(Journals, tr("Journal"));
    add(Publishers, tr("Publisher"));
    add(Conferences, tr("Conference"));
    add(Languages, tr("Language"));
    add(Formats, tr("Format"));
    add(ReadStatuses, tr("Read status"));
    add(Courts, tr("Court"));
    add(Jurisdictions, tr("Jurisdiction"));
    m_sections[Courts]->setHidden(true);
    m_sections[Jurisdictions]->setHidden(true);
    m_sections[Collections]->setExpanded(true);
    m_sections[Tags]->setExpanded(true);
    m_sections[Status]->setExpanded(true);
}

void FilterSidebar::applyTo(PublicationFilter &filter) const
{
    const auto strings = [this](Section section) { return QStringList(m_chosen[section].cbegin(), m_chosen[section].cend()); };
    const auto ids = [this](Section section) {
        QList<qint64> values;
        for (const QString &key : m_chosen[section])
            values << key.toLongLong();
        return values;
    };

    filter.tagIds = ids(Tags);
    filter.collectionIds = ids(Collections);
    filter.authorIds = ids(Authors);
    filter.journals = strings(Journals);
    filter.publishers = strings(Publishers);
    filter.conferences = strings(Conferences);
    filter.languages = strings(Languages);
    if (m_caseLawShown) {
        filter.courts = strings(Courts);
        filter.jurisdictions = strings(Jurisdictions);
    }
    filter.formats = strings(Formats);

    filter.readStatuses.clear();
    for (const QString &key : m_chosen[ReadStatuses]) {
        if (const auto status = readStatusFromDb(key))
            filter.readStatuses << *status;
    }

    const QSet<QString> &flags = m_chosen[Status];
    filter.needsReview = flags.contains(kFlagReview);
    filter.noText = flags.contains(kFlagNoText);
    filter.noFile = flags.contains(kFlagNoFile);
    filter.missingOrOffline = flags.contains(kFlagMissing);
    filter.unreadable = flags.contains(kFlagUnreadable);
    filter.retracted = flags.contains(kFlagRetracted);

    filter.yearMin = m_yearFrom->text().isEmpty() ? std::nullopt : std::optional<int>(m_yearFrom->text().toInt());
    filter.yearMax = m_yearTo->text().isEmpty() ? std::nullopt : std::optional<int>(m_yearTo->text().toInt());
}

bool FilterSidebar::hasSelection() const
{
    for (const QSet<QString> &chosen : m_chosen) {
        if (!chosen.isEmpty())
            return true;
    }
    return !m_yearFrom->text().isEmpty() || !m_yearTo->text().isEmpty();
}

bool FilterSidebar::onlyCollectionsChosen() const
{
    for (int section = 0; section < SectionCount; ++section) {
        if (section != Collections && !m_chosen[section].isEmpty())
            return false;
    }
    return !m_chosen[Collections].isEmpty() && m_yearFrom->text().isEmpty() && m_yearTo->text().isEmpty();
}

void FilterSidebar::clearSelection()
{
    if (!hasSelection())
        return;
    {
        const QSignalBlocker blocker(m_tree);
        for (int section = 0; section < SectionCount; ++section) {
            m_chosen[section].clear();
            m_chosenLabels[section].clear();
        }
        QTreeWidgetItemIterator it(m_tree);
        while (*it) {
            if ((*it)->flags() & Qt::ItemIsUserCheckable)
                (*it)->setCheckState(0, Qt::Unchecked);
            ++it;
        }
    }
    m_yearFrom->clear();
    m_yearTo->clear();
    updateClearButton();
    emit filterChanged();
}

void FilterSidebar::updateClearButton()
{
    m_clear->setEnabled(hasSelection());
}

QTreeWidgetItem *FilterSidebar::findValue(Section section, const QString &key) const
{
    QTreeWidgetItemIterator it(m_sections[section]);
    while (*it) {
        if ((*it)->data(0, kSectionRole).toInt() == section && (*it)->data(0, kKeyRole).toString() == key)
            return *it;
        ++it;
    }
    return nullptr;
}

bool FilterSidebar::isShown(int section) const
{
    return m_caseLawShown || (section != Courts && section != Jurisdictions);
}

void FilterSidebar::setCaseLawShown(bool shown)
{
    if (shown == m_caseLawShown)
        return;
    m_caseLawShown = shown;
    m_sections[Courts]->setHidden(!shown);
    m_sections[Jurisdictions]->setHidden(!shown);
}

void FilterSidebar::setValueChosen(Section section, const QString &key, bool chosen)
{
    if (QTreeWidgetItem *item = findValue(section, key))
        item->setCheckState(0, chosen ? Qt::Checked : Qt::Unchecked);
}

void FilterSidebar::setCount(QTreeWidgetItem *item, qint64 count)
{
    item->setText(1, QString::number(count));
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    item->setForeground(1, palette().color(QPalette::PlaceholderText));
}

void FilterSidebar::onItemChanged(QTreeWidgetItem *item)
{
    const int section = item->data(0, kSectionRole).toInt();
    if (section == kNoSection || !(item->flags() & Qt::ItemIsUserCheckable))
        return;
    const QString key = item->data(0, kKeyRole).toString();
    const bool checked = item->checkState(0) == Qt::Checked;
    if (checked == m_chosen[section].contains(key))
        return;
    if (checked) {
        m_chosen[section].insert(key);
        m_chosenLabels[section].insert(key, item->text(0));
    } else {
        m_chosen[section].remove(key);
        m_chosenLabels[section].remove(key);
    }
    updateClearButton();
    emit filterChanged();
}

void FilterSidebar::setData(const SidebarData &data)
{
    bool tagsRemoved = false;
    {
        const QSignalBlocker blocker(m_tree);
        for (int section = Authors; section < SectionCount; ++section) {
            const auto s = Section(section);
            updateFacet(s, data.facets.value(int(facetFor(s))));
        }

        // A chosen tag that's been deleted can't be unticked any other way.
        QSet<QString> existing;
        for (const TagCount &tag : data.tags)
            existing.insert(QString::number(tag.tagId));
        for (const QString &key : QSet<QString>(m_chosen[Tags])) {
            if (!existing.contains(key)) {
                m_chosen[Tags].remove(key);
                m_chosenLabels[Tags].remove(key);
                tagsRemoved = true;
            }
        }
        updateTags(data.tags);

        QSet<QString> collections;
        for (const CollectionCount &collection : data.collections)
            collections.insert(collection.key);
        for (const QString &key : QSet<QString>(m_chosen[Collections])) {
            if (!collections.contains(key)) {
                m_chosen[Collections].remove(key);
                m_chosenLabels[Collections].remove(key);
                tagsRemoved = true;
            }
        }
        updateCollections(data.collections);

        updateFlag(kFlagReview, tr("Needs checking"), data.flags.needsReview);
        updateFlag(kFlagNoText, tr("No text"), data.flags.noText);
        updateFlag(kFlagNoFile, tr("No file linked"), data.flags.noFile);
        updateFlag(kFlagMissing, tr("Missing or offline"), data.flags.missingOrOffline);
        updateFlag(kFlagUnreadable, tr("Can't be read"), data.flags.unreadable);
        updateFlag(kFlagRetracted, tr("Retracted or of concern"), data.flags.retracted);
        bool anyFlag = false;
        for (int i = 0; i < m_sections[Status]->childCount(); ++i)
            anyFlag = anyFlag || !m_sections[Status]->child(i)->isHidden();
        m_sections[Status]->setHidden(!anyFlag);
    }

    m_yearFrom->setPlaceholderText(data.years ? QString::number(data.years->first) : QString());
    m_yearTo->setPlaceholderText(data.years ? QString::number(data.years->second) : QString());

    if (tagsRemoved) {
        updateClearButton();
        emit filterChanged();
    }
}

void FilterSidebar::updateFacet(Section section, const QList<FacetValue> &values)
{
    QTreeWidgetItem *node = m_sections[section];

    struct Row {
        QString key;
        QString label;
        qint64 count;
    };
    QList<Row> rows;
    QSet<QString> shown;
    for (const FacetValue &value : values) {
        rows << Row{value.key, facetLabel(section, value), value.count};
        shown.insert(value.key);
    }
    // Something chosen stays on the list even if nothing matches it now, so it can be unticked.
    for (const QString &key : m_chosen[section]) {
        if (!shown.contains(key))
            rows << Row{key, m_chosenLabels[section].value(key, key), 0};
    }

    QHash<QString, QTreeWidgetItem *> existing;
    for (int i = node->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem *child = node->takeChild(i);
        const QString key = child->data(0, kKeyRole).toString();
        if (child->flags() & Qt::ItemIsUserCheckable)
            existing.insert(key, child);
        else
            delete child;
    }
    for (const Row &row : rows) {
        QTreeWidgetItem *item = existing.take(row.key);
        if (!item) {
            item = new QTreeWidgetItem;
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setData(0, kKeyRole, row.key);
            item->setData(0, kSectionRole, int(section));
        }
        item->setText(0, row.label);
        item->setToolTip(0, row.label);
        item->setCheckState(0, m_chosen[section].contains(row.key) ? Qt::Checked : Qt::Unchecked);
        setCount(item, row.count);
        node->addChild(item);
    }
    qDeleteAll(existing);

    if (section != Collections && values.size() >= kFacetLimit) {
        auto *note = new QTreeWidgetItem(node, {tr("Most common shown. Search for others.")});
        note->setFlags(Qt::ItemIsEnabled);
        note->setData(0, kSectionRole, kNoSection);
        note->setForeground(0, palette().color(QPalette::PlaceholderText));
    }
    node->setHidden(rows.isEmpty());
}

void FilterSidebar::updateCollections(const QList<CollectionCount> &collections)
{
    QList<TreeValue> values;
    for (const CollectionCount &collection : collections)
        values << TreeValue{collection.key.toLongLong(), collection.label, collection.parentId, std::nullopt, collection.count};
    updateTree(Collections, values, m_collectionSignature, tr("Collections you add appear here."));
    m_sections[Collections]->setHidden(false); // It's how anyone finds out about them.
}

void FilterSidebar::updateTags(const QList<TagCount> &tags)
{
    QList<TreeValue> values;
    for (const TagCount &tag : tags)
        values << TreeValue{tag.tagId, tag.name, tag.parentId, tag.colour, tag.count};
    updateTree(Tags, values, m_tagSignature, tr("Tags you add appear here."));
}

void FilterSidebar::updateTree(Section section, const QList<TreeValue> &values, QString &signature, const QString &hint)
{
    QTreeWidgetItem *node = m_sections[section];

    QString now = QString::number(int(section)) + u'\n';
    for (const TreeValue &value : values) {
        now += QStringLiteral("%1|%2|%3|%4\n")
                   .arg(value.id)
                   .arg(value.name)
                   .arg(value.parentId.value_or(0))
                   .arg(value.colour.value_or(QString()));
    }

    // Everything under the section, at any depth. Its hint, when there is one, is all it holds.
    const auto inSection = [node, section]() {
        QList<QTreeWidgetItem *> found;
        QList<QTreeWidgetItem *> waiting = {node};
        while (!waiting.isEmpty()) {
            QTreeWidgetItem *item = waiting.takeFirst();
            for (int i = 0; i < item->childCount(); ++i) {
                QTreeWidgetItem *child = item->child(i);
                if (child->data(0, kSectionRole).toInt() == int(section)) {
                    found << child;
                    waiting << child;
                }
            }
        }
        return found;
    };

    QHash<qint64, QTreeWidgetItem *> items;
    if (now != signature) {
        signature = now;
        QSet<QString> expanded;
        for (const QTreeWidgetItem *item : inSection()) {
            if (item->isExpanded())
                expanded.insert(item->data(0, kKeyRole).toString());
        }
        qDeleteAll(node->takeChildren());

        for (const TreeValue &value : values) {
            auto *item = new QTreeWidgetItem;
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setText(0, value.name);
            item->setToolTip(0, value.name);
            if (value.colour)
                item->setIcon(0, colourSwatch(*value.colour));
            const QString key = QString::number(value.id);
            item->setData(0, kKeyRole, key);
            item->setData(0, kSectionRole, int(section));
            item->setCheckState(0, m_chosen[section].contains(key) ? Qt::Checked : Qt::Unchecked);
            items.insert(value.id, item);
        }
        for (const TreeValue &value : values) {
            QTreeWidgetItem *parent = value.parentId ? items.value(*value.parentId, node) : node;
            parent->addChild(items.value(value.id));
        }
        for (const TreeValue &value : values) {
            if (expanded.contains(QString::number(value.id)))
                items.value(value.id)->setExpanded(true);
        }
    } else {
        for (QTreeWidgetItem *item : inSection())
            items.insert(item->data(0, kKeyRole).toLongLong(), item);
    }

    for (const TreeValue &value : values) {
        if (QTreeWidgetItem *item = items.value(value.id))
            setCount(item, value.count);
    }

    if (values.isEmpty() && node->childCount() == 0) {
        auto *note = new QTreeWidgetItem(node, {hint});
        note->setFlags(Qt::ItemIsEnabled);
        note->setData(0, kSectionRole, kNoSection);
        note->setForeground(0, palette().color(QPalette::PlaceholderText));
    }
}

QTreeWidgetItem *FilterSidebar::dropTarget(const QPoint &position) const
{
    QTreeWidgetItem *item = m_tree->itemAt(position);
    if (!item || !(item->flags() & Qt::ItemIsUserCheckable))
        return nullptr;
    const int section = item->data(0, kSectionRole).toInt();
    return section == Tags || section == Collections ? item : nullptr;
}

bool FilterSidebar::eventFilter(QObject *watched, QEvent *event)
{
    // Compared, not asked for: this sees the viewport's events while the tree is being destroyed.
    if (watched != m_treeViewport)
        return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        QTreeWidgetItem *target = drag->mimeData()->hasFormat(QString::fromLatin1(kPublicationDragMime))
            ? dropTarget(drag->position().toPoint())
            : nullptr;
        if (target) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            m_tree->setCurrentItem(target); // Shows where they'd go.
        } else if (event->type() == QEvent::DragEnter && drag->mimeData()->hasFormat(QString::fromLatin1(kPublicationDragMime))) {
            drag->accept(); // Over the tree, though not yet over a collection or tag.
        } else {
            drag->ignore();
        }
        return true;
    }
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        QTreeWidgetItem *target = drop->mimeData()->hasFormat(QString::fromLatin1(kPublicationDragMime))
            ? dropTarget(drop->position().toPoint())
            : nullptr;
        if (!target) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        emit publicationsDropped(Section(target->data(0, kSectionRole).toInt()), target->data(0, kKeyRole).toLongLong());
        return true;
    }
    default:
        return QWidget::eventFilter(watched, event);
    }
}

void FilterSidebar::updateFlag(const QString &key, const QString &label, qint64 count)
{
    QTreeWidgetItem *item = findValue(Status, key);
    if (!item) {
        item = new QTreeWidgetItem(m_sections[Status], {label});
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setData(0, kKeyRole, key);
        item->setData(0, kSectionRole, int(Status));
        item->setCheckState(0, Qt::Unchecked);
    }
    setCount(item, count);
    item->setHidden(count == 0 && !m_chosen[Status].contains(key));
}

void FilterSidebar::showContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = m_tree->itemAt(position);
    if (!item)
        return;
    if (item == m_savedSearches || item->parent() == m_savedSearches) {
        QMenu menu(this);
        menu.addAction(tr("Save the current search..."), this, &FilterSidebar::saveSearchRequested);
        if (item->data(0, kSectionRole).toInt() == kSavedSearch) {
            const qint64 id = item->data(0, kKeyRole).toLongLong();
            menu.addAction(tr("Replace with the current search"), this, [this, id] { emit savedSearchReplaceRequested(id); });
            menu.addSeparator();
            menu.addAction(tr("Rename..."), this, [this, id] { emit savedSearchRenameRequested(id); });
            menu.addAction(tr("Delete"), this, [this, id] { emit savedSearchDeleteRequested(id); });
        }
        menu.exec(m_tree->viewport()->mapToGlobal(position));
        return;
    }
    const bool isCollection = item->data(0, kSectionRole).toInt() == Collections;
    if (item == m_sections[Collections] || isCollection || (item->parent() == m_sections[Collections])) {
        QMenu menu(this);
        menu.addAction(tr("New collection..."), this, [this] { newCollection(); });
        if (isCollection) {
            const qint64 id = item->data(0, kKeyRole).toLongLong();
            menu.addAction(tr("New collection inside this one..."), this, [this, id] { newCollection(id); });
            menu.addSeparator();
            menu.addAction(tr("Rename..."), this, [this, id] { renameCollection(id); });
            menu.addAction(tr("Move..."), this, [this, id] { moveCollection(id); });
            menu.addSeparator();
            menu.addAction(tr("Delete..."), this, [this, id] { deleteCollection(id); });
        }
        menu.exec(m_tree->viewport()->mapToGlobal(position));
        return;
    }

    const bool isTag = item->data(0, kSectionRole).toInt() == Tags;
    if (item != m_sections[Tags] && !isTag)
        return;

    QMenu menu(this);
    menu.addAction(tr("New tag..."), this, [this] { newTag(std::nullopt); });
    if (isTag) {
        const qint64 id = item->data(0, kKeyRole).toLongLong();
        menu.addAction(tr("New tag inside this one..."), this, [this, id] { newTag(id); });
        menu.addSeparator();
        menu.addAction(tr("Rename..."), this, [this, id] { renameTag(id); });
        menu.addAction(tr("Choose colour..."), this, [this, id] { chooseColour(id); });
        menu.addAction(tr("Move..."), this, [this, id] { moveTag(id); });
        menu.addSeparator();
        menu.addAction(tr("Delete..."), this, [this, id] { deleteTag(id); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void FilterSidebar::newCollection(std::optional<qint64> parent)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New collection"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    try {
        m_collections.create(name, parent);
        emit collectionsChanged();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("New collection"), tr("That collection couldn't be created: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FilterSidebar::renameCollection(qint64 id)
{
    const auto collection = m_collections.get(id);
    if (!collection)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename collection"), tr("Name:"), QLineEdit::Normal, collection->name, &ok);
    if (!ok || name.trimmed().isEmpty() || name.trimmed() == collection->name)
        return;
    try {
        m_collections.rename(id, name);
        emit collectionsChanged();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Rename collection"), tr("That collection couldn't be renamed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FilterSidebar::deleteCollection(qint64 id)
{
    const auto collection = m_collections.get(id);
    if (!collection)
        return;
    const qsizetype inside = m_collections.withInside(id).size() - 1;
    const QString question = inside > 0
        ? tr("Delete the collection \"%1\" and the %2 inside it? The publications in them stay in the library.").arg(collection->name).arg(inside)
        : tr("Delete the collection \"%1\"? The publications in it stay in the library.").arg(collection->name);
    QMessageBox box(QMessageBox::Question, tr("Delete collection"), question, QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Yes)
        return;
    m_collections.remove(id);
    const QString key = QString::number(id);
    const bool wasChosen = m_chosen[Collections].remove(key);
    m_chosenLabels[Collections].remove(key);
    emit collectionsChanged();
    if (wasChosen) {
        updateClearButton();
        emit filterChanged();
    }
}

void FilterSidebar::moveCollection(qint64 id)
{
    const QList<qint64> own = m_collections.withInside(id);
    QStringList paths = {tr("(Top level)")};
    QList<qint64> ids = {0};
    for (const Collection &collection : m_collections.all()) {
        if (!own.contains(collection.id)) {
            paths << collection.path;
            ids << collection.id;
        }
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, tr("Move collection"), tr("Move to:"), paths, 0, false, &ok);
    if (!ok)
        return;
    const qint64 target = ids.at(paths.indexOf(chosen));
    try {
        m_collections.move(id, target == 0 ? std::nullopt : std::optional<qint64>(target));
        emit collectionsChanged();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Move collection"), tr("That collection couldn't be moved: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FilterSidebar::newTag(std::optional<qint64> parent)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New tag"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    try {
        m_tags.create(name.trimmed(), parent);
        emit tagsChanged();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("New tag"), tr("That tag couldn't be created: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FilterSidebar::renameTag(qint64 id)
{
    const auto tag = m_tags.get(id);
    if (!tag)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename tag"), tr("Name:"), QLineEdit::Normal, tag->name, &ok);
    if (!ok || name.trimmed().isEmpty() || name.trimmed() == tag->name)
        return;
    try {
        m_tags.rename(id, name.trimmed());
        emit tagsChanged();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Rename tag"), tr("That tag couldn't be renamed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FilterSidebar::chooseColour(qint64 id)
{
    const auto tag = m_tags.get(id);
    if (!tag)
        return;
    const QColor colour = QColorDialog::getColor(QColor(tag->colour), this, tr("Tag colour"));
    if (colour.isValid()) {
        m_tags.setColour(id, colour.name());
        emit tagsChanged();
    }
}

void FilterSidebar::moveTag(qint64 id)
{
    const QList<qint64> own = m_tags.withDescendants(id);
    QStringList paths = {tr("(Top level)")};
    QList<qint64> ids = {0};
    for (const Tag &tag : m_tags.all()) {
        if (!own.contains(tag.id)) {
            paths << m_tags.path(tag.id);
            ids << tag.id;
        }
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, tr("Move tag"), tr("Move to:"), paths, 0, false, &ok);
    if (!ok)
        return;
    const qint64 target = ids.at(paths.indexOf(chosen));
    try {
        m_tags.move(id, target == 0 ? std::nullopt : std::optional<qint64>(target));
        emit tagsChanged();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Move tag"), tr("That tag couldn't be moved: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FilterSidebar::deleteTag(qint64 id)
{
    const auto tag = m_tags.get(id);
    if (!tag)
        return;
    const qsizetype inside = m_tags.withDescendants(id).size() - 1;
    const QString question = inside > 0
        ? tr("Delete \"%1\" and the %2 tags inside it? Publications stay in the library.").arg(tag->name).arg(inside)
        : tr("Delete \"%1\"? Publications stay in the library.").arg(tag->name);
    QMessageBox box(QMessageBox::Question, tr("Delete tag"), question, QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    if (box.exec() != QMessageBox::Yes)
        return;
    m_tags.remove(id);
    emit tagsChanged();
}

} // namespace papertrail::ui
