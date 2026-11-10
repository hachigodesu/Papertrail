#pragma once

#include "core/query/PublicationFilter.h"
#include "ui/QueryThreads.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QWidget>

#include <optional>

class QLineEdit;
class QPoint;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::core {
class CollectionRepository;
class TagRepository;
}

namespace papertrail::ui {

// Everything chosen in the sidebar: for each section, each chosen value's key with its label, and the years.
struct SidebarChoices {
    QHash<int, QList<QPair<QString, QString>>> values;
    QString yearFrom;
    QString yearTo;
};

// The filters beside the library: tags, year range, and every value that can be narrowed down by,
// each with a live count. Values within one filter combine with OR, and filters with each other by AND.
class FilterSidebar : public QWidget {
    Q_OBJECT

public:
    enum Section {
        Tags,
        Status,
        Collections,
        Authors,
        Journals,
        Publishers,
        Conferences,
        Languages,
        Formats,
        ReadStatuses,
        Courts,        // Only on the Case Law tab: see setCaseLawShown.
        Jurisdictions,
        Ratings,
        SectionCount,
    };

    FilterSidebar(core::TagRepository &tags, core::CollectionRepository &collections, QWidget *parent = nullptr);

    void applyTo(core::PublicationFilter &filter) const;
    bool hasSelection() const;
    void clearSelection();
    bool onlyCollectionsChosen() const;

    // These ask what they need to, then say that collections have changed.
    void newCollection(std::optional<qint64> parent = std::nullopt);
    void moveCollection(qint64 id);
    void renameCollection(qint64 id);
    void deleteCollection(qint64 id);

    // Replaces the counts, and keeps whatever's currently chosen.
    void setData(const papertrail::ui::SidebarData &data);

    // Chooses or clears one value, as ticking it would.
    void setValueChosen(Section section, const QString &key, bool chosen);

    // Shows the filters only case law has, such as its court, for the Case Law tab. Elsewhere they're hidden, and what's
    // chosen in them neither applies nor is kept in a saved search, until they're shown again.
    void setCaseLawShown(bool shown);
    bool isCaseLawShown() const { return m_caseLawShown; }

    // What's chosen, and replacing all of it at once, as a saved search does. Values not listed yet are
    // chosen all the same, and shown with their labels.
    SidebarChoices choices() const;
    void setChoices(const SidebarChoices &choices);

    // The saved searches listed at the top, by id and name.
    void setSavedSearches(const QList<QPair<qint64, QString>> &searches);
    QTreeWidgetItem *savedSearchesItem() const { return m_savedSearches; }

    // How many values a section is showing, and its text and count, mainly for tests.
    QTreeWidget *tree() const { return m_tree; }
    QTreeWidgetItem *sectionItem(Section section) const { return m_sections[section]; }
    QTreeWidgetItem *findValue(Section section, const QString &key) const;

signals:
    void filterChanged();
    void tagsChanged();
    void collectionsChanged();
    // The publications chosen in the library were dragged onto a collection or tag.
    void publicationsDropped(Section section, qint64 id);
    void savedSearchChosen(qint64 id);
    void saveSearchRequested();
    void savedSearchReplaceRequested(qint64 id);
    void savedSearchRenameRequested(qint64 id);
    void savedSearchDeleteRequested(qint64 id);

private:
    bool m_caseLawShown = false;
    bool isShown(int section) const;
    void buildSections();
    void updateFacet(Section section, const QList<core::FacetValue> &values);
    struct TreeValue {
        qint64 id = 0;
        QString name;
        std::optional<qint64> parentId;
        std::optional<QString> colour; // For a tag, whose swatch is shown.
        qint64 count = 0;
    };
    // Tags and collections, each inside the one it belongs to.
    void updateTree(Section section, const QList<TreeValue> &values, QString &signature, const QString &hint);
    void updateTags(const QList<core::TagCount> &tags);
    void updateCollections(const QList<core::CollectionCount> &collections);
    bool eventFilter(QObject *watched, QEvent *event) override;
    // The collection or tag a drop at this place in the tree's viewport would land on.
    QTreeWidgetItem *dropTarget(const QPoint &position) const;
    void updateFlag(const QString &key, const QString &label, qint64 count);
    void setCount(QTreeWidgetItem *item, qint64 count);
    void onItemChanged(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &position);
    void newTag(std::optional<qint64> parent);
    void renameTag(qint64 id);
    void chooseColour(qint64 id);
    void moveTag(qint64 id);
    void deleteTag(qint64 id);
    void updateClearButton();

    core::TagRepository &m_tags;
    core::CollectionRepository &m_collections;
    QTreeWidget *m_tree = nullptr;
    QObject *m_treeViewport = nullptr; // What the event filter watches, kept so it never has to ask the tree.
    QLineEdit *m_yearFrom = nullptr;
    QLineEdit *m_yearTo = nullptr;
    QPushButton *m_clear = nullptr;
    QTreeWidgetItem *m_sections[SectionCount] = {};
    QTreeWidgetItem *m_savedSearches = nullptr;
    QSet<QString> m_chosen[SectionCount];
    QHash<QString, QString> m_chosenLabels[SectionCount]; // Chosen values may drop out of the results.
    QString m_tagSignature;
    QString m_collectionSignature;
};

} // namespace papertrail::ui
