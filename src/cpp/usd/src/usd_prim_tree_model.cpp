#include "usdcc/usd/usd_prim_tree_model.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_icons.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/stage.h>

#include <QColor>
#include <QFont>
#include <QMimeData>
#include <QStringList>

#include <algorithm>

namespace usdcc::usd {

namespace {
constexpr auto kPrimPathsMimeType = "application/x-usdcc-prim-paths";

std::vector<PXR_NS::SdfPath> draggedPaths(const QMimeData* data) {
    std::vector<PXR_NS::SdfPath> paths;
    const QString text = QString::fromUtf8(data->data(QString::fromLatin1(kPrimPathsMimeType)));
    for (const QString& path : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) paths.emplace_back(path.toStdString());
    return paths;
}
}  // namespace

namespace {
// Ancestors of the selection: the selection's own colour (dark.qss, QTreeView
// selection-background-color: accent blue at 18%) at half the opacity.
const QColor kAncestorBackground(61, 139, 253, 23);  // 9% of 255

// usdview's prim name colours (usdviewq/common.py UIPrimTypeColors). Other
// prims keep the theme's text colour (dark.qss, QTreeView color).
const QColor kInstanceColor(135, 206, 250);  // LIGHT_SKY_BLUE
const QColor kArcsColor(222, 158, 46);       // DARK_YELLOW
const QColor kPrototypeColor(118, 136, 217);
const QColor kTextColor(255, 255, 255, 191);
const QColor kUnusableColor(255, 255, 255, 64);
const QColor kWarningColor(0xff, 0xb3, 0x40);
// An inactive prim's colour, darker (usdviewq/primViewItem.py HALF_DARKER).
constexpr int kInactiveDarker = 150;

// The prims an instance's children are: its instance proxies.
const PXR_NS::Usd_PrimFlagsPredicate kChildren = PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate);

// As usdview's PrimInfo (usdviewq/utils.cpp).
bool hasCompositionArcs(const PXR_NS::UsdPrim& prim) {
    return prim.HasAuthoredReferences() || prim.HasAuthoredPayloads() || prim.HasAuthoredInherits() ||
           prim.HasAuthoredSpecializes() || prim.HasVariantSets();
}

bool inPrototype(const PXR_NS::UsdPrim& prim) { return prim.IsInPrototype() || prim.IsInstanceProxy(); }

// usdview's colour for the name, the first that applies; invalid for the
// theme's.
QColor nameColor(const PXR_NS::UsdPrim& prim) {
    if (prim.IsInstance()) return kInstanceColor;
    if (hasCompositionArcs(prim)) return kArcsColor;
    if (inPrototype(prim)) return kPrototypeColor;
    return {};
}

// usdview's tooltip line, e.g. "Inactive Defined Xform Prim".
QString describe(const PXR_NS::UsdPrim& prim) {
    QString text = QStringLiteral("Prim");
    if (!prim.GetTypeName().IsEmpty()) text = QString::fromStdString(prim.GetTypeName().GetString()) + ' ' + text;
    if (inPrototype(prim)) text = QStringLiteral("Prototype ") + text;
    if (!prim.IsDefined()) {
        text = QStringLiteral("Undefined ") + text;
    } else if (prim.IsAbstract()) {
        text = QStringLiteral("Abstract ") + text;
    } else {
        text = QStringLiteral("Defined ") + text;
    }
    if (!prim.IsActive()) {
        text = QStringLiteral("Inactive ") + text;
    } else if (prim.IsInstance()) {
        text = QStringLiteral("Instanced ") + text;
    }
    return text;
}
}  // namespace

UsdPrimTreeModel::UsdPrimTreeModel(QObject* parent) : QAbstractItemModel(parent) {
    m_root = std::make_unique<Node>();
    m_root->path = PXR_NS::SdfPath::AbsoluteRootPath();
}

void UsdPrimTreeModel::setStage(const PXR_NS::UsdStageRefPtr& stage) {
    m_stage = stage;
    refresh();
}

void UsdPrimTreeModel::setSelectedPaths(const std::vector<PXR_NS::SdfPath>& paths) {
    std::set<PXR_NS::SdfPath> ancestors;
    for (const PXR_NS::SdfPath& path : paths) {
        for (PXR_NS::SdfPath parent = path.GetParentPath();
             !parent.IsEmpty() && parent != PXR_NS::SdfPath::AbsoluteRootPath(); parent = parent.GetParentPath()) {
            ancestors.insert(parent);
        }
    }
    if (ancestors == m_selectionAncestors) {
        return;
    }
    // Repaint the rows that gain or lose the tint.
    std::set<PXR_NS::SdfPath> changed = ancestors;
    changed.insert(m_selectionAncestors.begin(), m_selectionAncestors.end());
    m_selectionAncestors = std::move(ancestors);
    for (const PXR_NS::SdfPath& path : changed) {
        const QModelIndex index = indexForPath(path);
        if (index.isValid()) {
            Q_EMIT dataChanged(index, index, {Qt::BackgroundRole});
        }
    }
}

void UsdPrimTreeModel::refresh() {
    beginResetModel();
    m_root = std::make_unique<Node>();
    m_root->path = PXR_NS::SdfPath::AbsoluteRootPath();
    if (m_stage) {
        buildChildren(m_root.get());
    }
    endResetModel();
}

void UsdPrimTreeModel::buildChildren(Node* node) {
    const PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(node->path);
    if (!prim) {
        return;
    }
    for (const auto& child : prim.GetFilteredChildren(kChildren)) {
        auto childNode = std::make_unique<Node>();
        childNode->path = child.GetPath();
        childNode->parent = node;
        if (child.IsInstance()) {
            childNode->unfetched = true;  // its instance proxies come with fetchMore()
        } else {
            buildChildren(childNode.get());
        }
        node->children.push_back(std::move(childNode));
    }
}

UsdPrimTreeModel::Node* UsdPrimTreeModel::nodeForIndex(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : m_root.get();
}

QModelIndex UsdPrimTreeModel::indexForNode(Node* node) const {
    return node == m_root.get() ? QModelIndex() : indexForPath(node->path);
}

bool UsdPrimTreeModel::hasChildren(const QModelIndex& parent) const {
    if (parent.column() > 0) return false;
    const Node* node = nodeForIndex(parent);
    if (!node->unfetched) return !node->children.empty();
    const PXR_NS::UsdPrim prim = m_stage ? m_stage->GetPrimAtPath(node->path) : PXR_NS::UsdPrim();
    return prim && !prim.GetFilteredChildren(kChildren).empty();
}

bool UsdPrimTreeModel::canFetchMore(const QModelIndex& parent) const {
    return parent.column() <= 0 && nodeForIndex(parent)->unfetched;
}

void UsdPrimTreeModel::fetchMore(const QModelIndex& parent) {
    Node* node = nodeForIndex(parent);
    if (!node->unfetched || !m_stage) return;
    node->unfetched = false;
    Node loaded;
    loaded.path = node->path;
    buildChildren(&loaded);
    if (loaded.children.empty()) return;
    beginInsertRows(parent, 0, static_cast<int>(loaded.children.size()) - 1);
    for (auto& child : loaded.children) child->parent = node;
    node->children = std::move(loaded.children);
    endInsertRows();
}

QModelIndex UsdPrimTreeModel::fetchPath(const PXR_NS::SdfPath& path) {
    Node* node = m_root.get();
    for (const PXR_NS::SdfPath& prefix : path.GetPrefixes()) {
        if (node->unfetched) fetchMore(indexForNode(node));
        auto it = std::find_if(node->children.begin(), node->children.end(),
                               [&](const std::unique_ptr<Node>& child) { return child->path == prefix; });
        if (it == node->children.end()) return {};
        node = it->get();
    }
    return indexForNode(node);
}

UsdPrimTreeModel::Node* UsdPrimTreeModel::findNode(Node* node, const PXR_NS::SdfPath& path) const {
    if (node->path == path) {
        return node;
    }
    for (auto& child : node->children) {
        if (Node* found = findNode(child.get(), path)) {
            return found;
        }
    }
    return nullptr;
}

PXR_NS::SdfPath UsdPrimTreeModel::pathForIndex(const QModelIndex& index) const {
    if (!index.isValid()) {
        return PXR_NS::SdfPath();
    }
    return static_cast<Node*>(index.internalPointer())->path;
}

QModelIndex UsdPrimTreeModel::indexForPath(const PXR_NS::SdfPath& path) const {
    Node* node = findNode(m_root.get(), path);
    if (!node || node == m_root.get() || !node->parent) {
        return QModelIndex();
    }
    Node* parentNode = node->parent;
    auto it = std::find_if(parentNode->children.begin(), parentNode->children.end(),
                            [node](const std::unique_ptr<Node>& n) { return n.get() == node; });
    if (it == parentNode->children.end()) {
        return QModelIndex();
    }
    const int row = static_cast<int>(std::distance(parentNode->children.begin(), it));
    return createIndex(row, 0, node);
}

QModelIndex UsdPrimTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }
    Node* parentNode = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : m_root.get();
    if (!parentNode || row < 0 || static_cast<size_t>(row) >= parentNode->children.size()) {
        return QModelIndex();
    }
    return createIndex(row, column, parentNode->children[row].get());
}

QModelIndex UsdPrimTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return QModelIndex();
    }
    Node* node = static_cast<Node*>(child.internalPointer());
    Node* parentNode = node->parent;
    if (!parentNode || parentNode == m_root.get() || !parentNode->parent) {
        return QModelIndex();
    }
    Node* grandparent = parentNode->parent;
    auto it = std::find_if(grandparent->children.begin(), grandparent->children.end(),
                            [parentNode](const std::unique_ptr<Node>& n) { return n.get() == parentNode; });
    const int row = static_cast<int>(std::distance(grandparent->children.begin(), it));
    return createIndex(row, 0, parentNode);
}

int UsdPrimTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    Node* node = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : m_root.get();
    return node ? static_cast<int>(node->children.size()) : 0;
}

int UsdPrimTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant UsdPrimTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || !m_stage) {
        return {};
    }
    Node* node = static_cast<Node*>(index.internalPointer());
    const PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(node->path);
    if (!prim) {
        return {};
    }

    switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            return QString::fromStdString(node->path.GetName());
        case Qt::DecorationRole:
            return prim_icons::icon(prim);  // faded for an inactive prim
        case Qt::ForegroundRole: {
            if (m_rowStatus) {
                const RowStatus status = m_rowStatus(prim);
                if (!status.usable) return kUnusableColor;
                if (status.warning) return kWarningColor;
            }
            const QColor color = nameColor(prim);
            if (!prim.IsActive()) return (color.isValid() ? color : kTextColor).darker(kInactiveDarker);
            if (color.isValid()) return color;
            break;  // the theme's
        }
        case Qt::BackgroundRole:
            if (isSelectionAncestor(node->path)) {
                return kAncestorBackground;
            }
            break;
        case Qt::FontRole: {
            // usdview's UIFonts: the specifier, by weight. Only these are set;
            // the view's font gives the rest (size, family).
            QFont font;
            if (prim.IsAbstract()) {
                font.setWeight(QFont::Normal);
            } else if (!prim.IsDefined()) {
                font.setWeight(QFont::Light);
                font.setItalic(true);
            } else {
                font.setWeight(QFont::Bold);
            }
            return font;
        }
        case Qt::CheckStateRole:
            if (m_readOnly || prim.IsInstanceProxy()) break;  // read-only: no checkbox
            return prim.IsActive() ? Qt::Checked : Qt::Unchecked;
        case Qt::ToolTipRole: {
            QString tip = QString::fromStdString(node->path.GetString()) + QLatin1Char('\n') + describe(prim);
            if (hasCompositionArcs(prim)) tip += tr("\nHas composition arcs");
            if (m_rowStatus) {
                const RowStatus status = m_rowStatus(prim);
                if (!status.note.isEmpty()) tip += QLatin1Char('\n') + status.note;
            }
            return tip;
        }
        default:
            break;
    }
    return {};
}

bool UsdPrimTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || !m_stage || m_readOnly) {
        return false;
    }
    Node* node = static_cast<Node*>(index.internalPointer());
    PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(node->path);
    if (!prim_actions::isEditable(prim)) {
        return false;  // an instance proxy, or a prim outside the variant being edited
    }

    // Not edited here: the panel makes each an undo step (prim_actions); the
    // stage's change notice then rebuilds the tree.
    if (role == Qt::EditRole) {
        const std::string newName = value.toString().toStdString();
        if (newName.empty() || newName == node->path.GetName() || !PXR_NS::SdfPath::IsValidIdentifier(newName)) {
            return false;
        }
        Q_EMIT renameRequested(node->path, newName);
        return true;
    }

    if (role == Qt::CheckStateRole) {
        Q_EMIT activeChangeRequested(node->path, value.toInt() == Qt::Checked);
        return true;
    }

    return false;
}

Qt::ItemFlags UsdPrimTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        // The empty space below the rows: the root.
        return m_stage && !m_readOnly ? Qt::ItemIsDropEnabled : Qt::NoItemFlags;
    }
    const PXR_NS::UsdPrim prim = m_stage ? m_stage->GetPrimAtPath(pathForIndex(index)) : PXR_NS::UsdPrim();
    // Enabled, so a greyed row can still be expanded to reach what's under it.
    if (prim && m_rowStatus && !m_rowStatus(prim).usable) return Qt::ItemIsEnabled;
    if (m_readOnly || (prim && prim.IsInstanceProxy())) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;  // read-only
    }
    if (prim && !prim_actions::isEditable(prim)) return Qt::ItemIsEnabled | Qt::ItemIsSelectable;  // outside a variant being edited
    // Inside a variant being edited a prim can be activated, but not renamed.
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    if (!prim || prim_actions::canRename(prim)) flags |= Qt::ItemIsEditable;
    return flags;
}

QStringList UsdPrimTreeModel::mimeTypes() const { return {QString::fromLatin1(kPrimPathsMimeType)}; }

QMimeData* UsdPrimTreeModel::mimeData(const QModelIndexList& indexes) const {
    QStringList paths;
    for (const QModelIndex& index : indexes) {
        const QString path = QString::fromStdString(pathForIndex(index).GetString());
        if (!paths.contains(path)) paths << path;
    }
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(kPrimPathsMimeType), paths.join(QLatin1Char('\n')).toUtf8());
    return data;
}

Qt::DropActions UsdPrimTreeModel::supportedDropActions() const { return Qt::MoveAction; }

bool UsdPrimTreeModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int, int, const QModelIndex& parent) const {
    if (!m_stage || m_readOnly || action != Qt::MoveAction || !data || !data->hasFormat(QString::fromLatin1(kPrimPathsMimeType))) return false;
    const QString why = prim_actions::whyNotDrop(m_stage, draggedPaths(data), parent.isValid() ? pathForIndex(parent) : PXR_NS::SdfPath::AbsoluteRootPath());
    Q_EMIT dropHint(why);
    return why.isEmpty();
}

bool UsdPrimTreeModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) return false;
    Q_EMIT dropRequested(draggedPaths(data), parent.isValid() ? pathForIndex(parent) : PXR_NS::SdfPath::AbsoluteRootPath(), row);
    return true;
}

QVariant UsdPrimTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && section == 0 && role == Qt::DisplayRole) {
        return tr("Name");
    }
    return {};
}

}  // namespace usdcc::usd
