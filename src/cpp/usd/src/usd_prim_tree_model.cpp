#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/usd/namespaceEditor.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/stage.h>

#include <QColor>
#include <QFont>

#include <algorithm>

namespace usdcc::usd {

UsdPrimTreeModel::UsdPrimTreeModel(QObject* parent) : QAbstractItemModel(parent) {
    m_root = std::make_unique<Node>();
    m_root->path = PXR_NS::SdfPath::AbsoluteRootPath();
}

void UsdPrimTreeModel::setStage(const PXR_NS::UsdStageRefPtr& stage) {
    m_stage = stage;
    refresh();
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
    for (const auto& child : prim.GetFilteredChildren(PXR_NS::UsdPrimAllPrimsPredicate)) {
        auto childNode = std::make_unique<Node>();
        childNode->path = child.GetPath();
        childNode->parent = node;
        buildChildren(childNode.get());
        node->children.push_back(std::move(childNode));
    }
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
        case Qt::ForegroundRole:
            if (!prim.IsActive()) {
                return QColor(Qt::gray);
            }
            break;
        case Qt::FontRole:
            if (!prim.IsActive()) {
                QFont font;
                font.setItalic(true);
                return font;
            }
            break;
        case Qt::CheckStateRole:
            return prim.IsActive() ? Qt::Checked : Qt::Unchecked;
        case Qt::ToolTipRole:
            return QString::fromStdString(node->path.GetString());
        default:
            break;
    }
    return {};
}

bool UsdPrimTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || !m_stage) {
        return false;
    }
    Node* node = static_cast<Node*>(index.internalPointer());
    PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(node->path);
    if (!prim) {
        return false;
    }

    if (role == Qt::EditRole) {
        const std::string newName = value.toString().toStdString();
        if (newName.empty() || newName == node->path.GetName() || !PXR_NS::SdfPath::IsValidIdentifier(newName)) {
            return false;
        }

        PXR_NS::UsdNamespaceEditor editor(m_stage);
        const PXR_NS::SdfPath oldPath = node->path;
        if (!editor.RenamePrim(prim, PXR_NS::TfToken(newName)) || !editor.ApplyEdits()) {
            return false;
        }
        const PXR_NS::SdfPath newPath = oldPath.GetParentPath().AppendChild(PXR_NS::TfToken(newName));
        emit primRenamed(oldPath, newPath);
        refresh();
        return true;
    }

    if (role == Qt::CheckStateRole) {
        prim.SetActive(value.toInt() == Qt::Checked);
        emit dataChanged(index, index, {Qt::CheckStateRole, Qt::ForegroundRole, Qt::FontRole});
        return true;
    }

    return false;
}

Qt::ItemFlags UsdPrimTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsUserCheckable;
}

QVariant UsdPrimTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && section == 0 && role == Qt::DisplayRole) {
        return tr("Name");
    }
    return {};
}

}  // namespace usdcc::usd
