#pragma once

#include "core/query/PublicationFilter.h"
#include "ui/QueryThreads.h"

#include <QHash>
#include <QSet>
#include <QWidget>

#include <optional>

class QLineEdit;
class QPoint;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::core {
class TagRepository;
}

namespace papertrail::ui {

// The filters beside the library: tags, year range, and every value that can be narrowed down by,
// each with a live count. Values within one filter combine with OR, and filters with each other by AND.
class FilterSidebar : public QWidget {
    Q_OBJECT

public:
    enum Section {
        Tags,
        Status,
        Authors,
        Journals,
        Publishers,
        Conferences,
        Institutions,
        Languages,
        Formats,
        ReadStatuses,
        SectionCount,
    };

    explicit FilterSidebar(core::TagRepository &tags, QWidget *parent = nullptr);

    void applyTo(core::PublicationFilter &filter) const;
    bool hasSelection() const;
    void clearSelection();

    // Replaces the counts, and keeps whatever's currently chosen.
    void setData(const papertrail::ui::SidebarData &data);

    // Chooses or clears one value, as ticking it would.
    void setValueChosen(Section section, const QString &key, bool chosen);

    // How many values a section is showing, and its text and count, mainly for tests.
    QTreeWidget *tree() const { return m_tree; }
    QTreeWidgetItem *sectionItem(Section section) const { return m_sections[section]; }
    QTreeWidgetItem *findValue(Section section, const QString &key) const;

signals:
    void filterChanged();
    void tagsChanged();

private:
    void buildSections();
    void updateFacet(Section section, const QList<core::FacetValue> &values);
    void updateTags(const QList<core::TagCount> &tags);
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
    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_yearFrom = nullptr;
    QLineEdit *m_yearTo = nullptr;
    QPushButton *m_clear = nullptr;
    QTreeWidgetItem *m_sections[SectionCount] = {};
    QSet<QString> m_chosen[SectionCount];
    QHash<QString, QString> m_chosenLabels[SectionCount]; // Chosen values may drop out of the results.
    QString m_tagSignature;
};

} // namespace papertrail::ui
