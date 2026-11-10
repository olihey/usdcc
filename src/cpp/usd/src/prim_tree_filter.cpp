#include "usdcc/usd/prim_tree_filter.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/theme.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/usd/primRange.h>

#include <QAction>
#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
// A menu that stays open when a box in it is ticked, so several types can be picked in a row.
class StayOpenMenu : public QMenu {
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        QAction* action = actionAt(event->position().toPoint());
        if (action && action->isCheckable() && action->isEnabled()) {
            action->trigger();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
};

constexpr auto kTypeProperty = "primType";

// The icon in the accent colour: the menu draws an icon where a tick would go, so a ticked
// type would otherwise look like any other.
QIcon tinted(const QIcon& icon, const QColor& color) {
    QIcon result;
    for (const int size : {16, 24, 32, 48}) {
        QPixmap pixmap = icon.pixmap(size, size);
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

// Ticked: bold, and the icon in the accent colour.
void showTicked(QAction* action, const QIcon& icon) {
    QFont font = action->font();
    font.setBold(action->isChecked());
    action->setFont(font);
    action->setIcon(action->isChecked() ? tinted(icon, usdcc::ui::theme::accent()) : icon);
}

QString typeLabel(const std::string& type) { return type.empty() ? PrimTreeFilterBar::tr("No Type") : QString::fromStdString(type); }
}  // namespace

PrimTreeFilterProxy::PrimTreeFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(true);  // a match keeps its ancestors
}

void PrimTreeFilterProxy::setPrimModel(UsdPrimTreeModel* model) {
    m_model = model;
    setSourceModel(model);
}

void PrimTreeFilterProxy::setNameFilter(const QString& text) {
    if (text == m_name) return;
    m_name = text;
    invalidateRowsFilter();
}

void PrimTreeFilterProxy::setTypeFilter(std::optional<std::set<std::string>> types) {
    if (types == m_types) return;
    m_types = std::move(types);
    invalidateRowsFilter();
}

bool PrimTreeFilterProxy::matches(const QModelIndex& sourceIndex) const {
    if (!m_model || !sourceIndex.isValid()) return false;
    if (!m_name.isEmpty() && !sourceIndex.data(Qt::DisplayRole).toString().contains(m_name, Qt::CaseInsensitive)) return false;
    if (!m_types) return true;
    const UsdStageRefPtr stage = m_model->stage();
    const UsdPrim prim = stage ? stage->GetPrimAtPath(m_model->pathForIndex(sourceIndex)) : UsdPrim();
    return prim && m_types->count(prim.GetTypeName().GetString()) > 0;
}

bool PrimTreeFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (!isFiltering()) return true;
    return matches(sourceModel()->index(sourceRow, 0, sourceParent));
}

int PrimTreeFilterProxy::matchCount() const {
    int count = 0;
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < rowCount(parent); ++row) {
            const QModelIndex index = this->index(row, 0, parent);
            if (matches(mapToSource(index))) ++count;
            pending.push_back(index);
        }
    }
    return count;
}

PrimTreeFilterBar::PrimTreeFilterBar(QTreeView* tree, PrimTreeFilterProxy* proxy, QWidget* parent)
    : QWidget(parent), m_tree(tree), m_proxy(proxy) {
    setObjectName(QStringLiteral("primTreeFilterBar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    usdcc::ui::scaled::spacing(layout, 4);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("primSearch"));
    m_search->setPlaceholderText(tr("Search prims by name"));
    m_search->setClearButtonEnabled(true);
    m_typesButton = new QToolButton(this);
    m_typesButton->setObjectName(QStringLiteral("primTypesButton"));
    m_typesButton->setPopupMode(QToolButton::InstantPopup);
    m_typesButton->setToolTip(tr("Show only prims of some types, of those this stage has"));
    m_typesMenu = new StayOpenMenu(m_typesButton);
    m_typesButton->setMenu(m_typesMenu);
    layout->addWidget(m_search, 1);
    layout->addWidget(m_typesButton);
    updateTypesButton();

    // The stage may have changed since: read it each time.
    connect(m_typesMenu, &QMenu::aboutToShow, this, &PrimTreeFilterBar::fillTypesMenu);
    m_typingTimer = new QTimer(this);
    m_typingTimer->setSingleShot(true);
    m_typingTimer->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, m_typingTimer, qOverload<>(&QTimer::start));
    connect(m_typingTimer, &QTimer::timeout, this, [this]() {
        remember();
        m_proxy->setNameFilter(m_search->text());
        apply();
    });
}

std::vector<std::pair<std::string, int>> PrimTreeFilterBar::stageTypes() const {
    std::map<std::string, int> counts;
    UsdPrimTreeModel* model = m_proxy->primModel();
    const UsdStageRefPtr stage = model ? model->stage() : UsdStageRefPtr();
    if (stage) {
        // Inactive prims too, as the tree has them.
        for (const UsdPrim& prim : stage->TraverseAll()) ++counts[prim.GetTypeName().GetString()];
    }
    return {counts.begin(), counts.end()};
}

void PrimTreeFilterBar::fillTypesMenu() {
    m_typesMenu->clear();
    const std::optional<std::set<std::string>>& chosen = m_proxy->typeFilter();
    QAction* all = m_typesMenu->addAction(tr("All Types"));
    all->setCheckable(true);
    // The menu stays open, so its other entries have to show they no longer apply.
    auto ticked = std::make_shared<std::vector<std::pair<QAction*, QIcon>>>();
    connect(all, &QAction::triggered, this, [this, all, ticked]() {
        for (const auto& [action, icon] : *ticked) {
            const QSignalBlocker quiet(action);
            action->setChecked(false);
            showTicked(action, icon);
        }
        all->setChecked(true);  // clicking it again doesn't untick it
        QFont font = all->font();
        font.setBold(true);
        all->setFont(font);
        setTypeFilter(std::nullopt);
    });
    all->setChecked(!chosen);
    QFont allFont = all->font();
    allFont.setBold(!chosen);
    all->setFont(allFont);
    m_typesMenu->addSeparator();
    for (const auto& [type, count] : stageTypes()) {
        QAction* action = m_typesMenu->addAction(QStringLiteral("%1  (%2)").arg(typeLabel(type)).arg(count));
        const QIcon icon = prim_icons::icon(prim_icons::iconNameForType(TfToken(type)));
        action->setCheckable(true);
        action->setChecked(chosen && chosen->count(type));
        showTicked(action, icon);
        ticked->push_back({action, icon});
        action->setProperty(kTypeProperty, QString::fromStdString(type));
        connect(action, &QAction::toggled, this, [this, type, all, action, icon](bool on) {
            showTicked(action, icon);
            std::set<std::string> types = m_proxy->typeFilter().value_or(std::set<std::string>());
            if (on) {
                types.insert(type);
            } else {
                types.erase(type);
            }
            all->setChecked(types.empty());
            QFont font = all->font();
            font.setBold(types.empty());
            all->setFont(font);
            setTypeFilter(types.empty() ? std::nullopt : std::optional<std::set<std::string>>(types));
        });
    }
    if (stageTypes().empty()) m_typesMenu->addAction(tr("No prims"))->setEnabled(false);
}

void PrimTreeFilterBar::updateTypesButton() {
    const std::optional<std::set<std::string>>& chosen = m_proxy->typeFilter();
    QStringList names;
    if (chosen) {
        for (const std::string& type : *chosen) names << typeLabel(type);
    }
    // Their names, as far as two; the tooltip has them all.
    if (!chosen) {
        m_typesButton->setText(tr("All Types"));
    } else if (names.size() <= 2) {
        m_typesButton->setText(names.join(QStringLiteral(", ")));
    } else {
        m_typesButton->setText(tr("%1, %2 +%3").arg(names[0], names[1]).arg(names.size() - 2));
    }
    m_typesButton->setToolTip(chosen ? tr("Showing only: %1").arg(names.join(QStringLiteral(", "))) : tr("Show only prims of some types, of those this stage has"));
    // Lit while it's hiding something.
    m_typesButton->setCheckable(true);
    m_typesButton->setChecked(chosen.has_value());
}

void PrimTreeFilterBar::setNameFilter(const QString& text) {
    m_typingTimer->stop();
    {
        const QSignalBlocker quiet(m_search);
        m_search->setText(text);
    }
    remember();
    m_proxy->setNameFilter(text);
    apply();
}

void PrimTreeFilterBar::setTypeFilter(std::optional<std::set<std::string>> types) {
    remember();
    m_proxy->setTypeFilter(std::move(types));
    updateTypesButton();
    apply();
}

void PrimTreeFilterBar::clear() {
    m_typingTimer->stop();
    {
        const QSignalBlocker quiet(m_search);
        m_search->clear();
    }
    remember();
    m_proxy->setNameFilter(QString());
    m_proxy->setTypeFilter(std::nullopt);
    updateTypesButton();
    apply();
}

bool PrimTreeFilterBar::isFiltering() const { return m_proxy->isFiltering(); }

void PrimTreeFilterBar::remember() {
    Q_EMIT filterAboutToChange();
    // Before the filter changes what shows: what it hides was open too.
    if (!m_filtering) m_expandedBefore = expandedPaths();
}

void PrimTreeFilterBar::apply() {
    const bool filtering = m_proxy->isFiltering();
    const bool was = m_filtering;
    m_filtering = filtering;
    if (filtering) {
        m_tree->expandAll();  // every match shows
    } else if (was) {
        m_tree->collapseAll();
        expandPaths(m_expandedBefore);
    }
    Q_EMIT filterApplied();
}

void PrimTreeFilterBar::reapply() {
    if (m_filtering) m_tree->expandAll();
}

std::set<SdfPath> PrimTreeFilterBar::expandedPaths() const {
    std::set<SdfPath> paths;
    UsdPrimTreeModel* model = m_proxy->primModel();
    if (!model) return paths;
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (!m_tree->isExpanded(index)) continue;  // what's under it is hidden anyway
            paths.insert(model->pathForIndex(m_proxy->mapToSource(index)));
            pending.push_back(index);
        }
    }
    return paths;
}

void PrimTreeFilterBar::expandPaths(const std::set<SdfPath>& paths) {
    UsdPrimTreeModel* model = m_proxy->primModel();
    if (!model) return;
    for (const SdfPath& path : paths) {
        const QModelIndex index = m_proxy->mapFromSource(model->fetchPath(path));
        if (index.isValid()) m_tree->setExpanded(index, true);
    }
}

}  // namespace usdcc::usd
