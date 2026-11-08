#pragma once

#include "usdcc/core/export.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <QSortFilterProxyModel>
#include <QString>
#include <QWidget>

#include <optional>
#include <set>
#include <string>

class QLineEdit;
class QMenu;
class QTimer;
class QToolButton;
class QTreeView;

namespace usdcc::usd {

class UsdPrimTreeModel;

// Which prims of a UsdPrimTreeModel show: those whose name contains a text and whose type
// is one of a set, with their ancestors, so a match is always where it is in the tree.
class USDCC_USD_UI_API PrimTreeFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit PrimTreeFilterProxy(QObject* parent = nullptr);

    void setPrimModel(UsdPrimTreeModel* model);
    UsdPrimTreeModel* primModel() const { return m_model; }

    // Case doesn't matter. Empty: any name.
    void setNameFilter(const QString& text);
    // Their type names, "" for a prim with none. No value: any type.
    void setTypeFilter(std::optional<std::set<std::string>> types);
    const QString& nameFilter() const { return m_name; }
    const std::optional<std::set<std::string>>& typeFilter() const { return m_types; }
    bool isFiltering() const { return !m_name.isEmpty() || m_types.has_value(); }

    // Whether this row of the source model matches, rather than being shown for one below it.
    bool matches(const QModelIndex& sourceIndex) const;
    // The rows that match, not counting the ancestors shown for them.
    int matchCount() const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    UsdPrimTreeModel* m_model = nullptr;
    QString m_name;
    std::optional<std::set<std::string>> m_types;
};

// A search field and a menu of the prim types the stage uses, above a prim tree: what the
// Outliner, Show Prims and the relationship target dialog show. While it filters, the tree is
// open all the way down; clearing it puts back what was open before.
class USDCC_USD_UI_API PrimTreeFilterBar : public QWidget {
    Q_OBJECT

public:
    PrimTreeFilterBar(QTreeView* tree, PrimTreeFilterProxy* proxy, QWidget* parent = nullptr);

    QLineEdit* searchEdit() const { return m_search; }
    QToolButton* typesButton() const { return m_typesButton; }
    // The types the stage has, and how many of each, as the menu lists them. "" is no type.
    std::vector<std::pair<std::string, int>> stageTypes() const;

    // At once, where typing waits for a pause.
    void setNameFilter(const QString& text);
    void setTypeFilter(std::optional<std::set<std::string>> types);
    void clear();
    bool isFiltering() const;

    // After the tree's model was rebuilt: open everything again while filtering.
    void reapply();

Q_SIGNALS:
    void filterApplied();

private:
    void remember();
    void apply();
    void fillTypesMenu();
    void updateTypesButton();
    std::set<PXR_NS::SdfPath> expandedPaths() const;
    void expandPaths(const std::set<PXR_NS::SdfPath>& paths);

    QTreeView* m_tree;
    PrimTreeFilterProxy* m_proxy;
    QLineEdit* m_search = nullptr;
    QToolButton* m_typesButton = nullptr;
    QMenu* m_typesMenu = nullptr;
    QTimer* m_typingTimer = nullptr;
    bool m_filtering = false;
    std::set<PXR_NS::SdfPath> m_expandedBefore;
};

}  // namespace usdcc::usd
