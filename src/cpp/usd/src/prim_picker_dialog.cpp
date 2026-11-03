#include "usdcc/usd/prim_picker_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/prim_icons.h"

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <map>

namespace usdcc::usd {

namespace {
constexpr int kPathRole = Qt::UserRole;

QString qs(const std::string& text) { return QString::fromStdString(text); }
}  // namespace

PrimPickerDialog::PrimPickerDialog(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfPath& current,
                                   QWidget* parent)
    : QDialog(parent) {
    const QString file = stage ? QFileInfo(qs(stage->GetRootLayer()->GetRealPath())).fileName() : QString();
    setWindowTitle(file.isEmpty() ? tr("Select Prim") : tr("Select Prim - %1").arg(file));
    auto* layout = new QVBoxLayout(this);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter by name"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({tr("Prim"), tr("Type")});
    const PXR_NS::TfToken defaultPrim = stage ? stage->GetDefaultPrim().GetName() : PXR_NS::TfToken();
    std::map<PXR_NS::SdfPath, QStandardItem*> items;
    if (stage) {
        for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange::Stage(stage, PXR_NS::UsdPrimAllPrimsPredicate)) {
            auto* name = new QStandardItem(prim_icons::icon(prim), qs(prim.GetName().GetString()));
            name->setData(qs(prim.GetPath().GetString()), kPathRole);
            name->setToolTip(qs(prim.GetPath().GetString()));
            name->setEditable(false);
            auto* type = new QStandardItem(qs(prim.GetTypeName().GetString()));
            type->setEditable(false);
            if (prim.GetPath().GetParentPath().IsAbsoluteRootPath() && prim.GetName() == defaultPrim) {
                QFont bold = name->font();
                bold.setBold(true);
                name->setFont(bold);
                name->setToolTip(tr("%1 (the default prim)").arg(name->toolTip()));
            }
            const auto parentItem = items.find(prim.GetPath().GetParentPath());
            QStandardItem* under = parentItem == items.end() ? m_model->invisibleRootItem() : parentItem->second;
            under->appendRow({name, type});
            items[prim.GetPath()] = name;
        }
    }

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setRecursiveFilteringEnabled(true);  // a match keeps its ancestors
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(0);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    usdcc::ui::scaled::iconSize(m_tree, 16, 16);
    layout->addWidget(m_tree, 1);

    auto* buttons = new QDialogButtonBox(this);
    m_use = buttons->addButton(tr("Use Prim"), QDialogButtonBox::AcceptRole);
    m_use->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_filter, &QLineEdit::textChanged, this, &PrimPickerDialog::setFilter);
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { m_use->setEnabled(!selectedPath().IsEmpty()); });
    connect(m_tree, &QTreeView::doubleClicked, this, [this]() {
        if (!selectedPath().IsEmpty()) accept();
    });

    // The top level open, and the current prim (or the default one) selected.
    for (int row = 0; row < m_proxy->rowCount(); ++row) m_tree->expand(m_proxy->index(row, 0));
    m_use->setEnabled(false);
    selectPath(!current.IsEmpty() ? current
                                  : (defaultPrim.IsEmpty() ? PXR_NS::SdfPath()
                                                           : PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(defaultPrim)));
    m_filter->setFocus();
    resize(usdcc::ui::scaled::size(420, 520));
}

PXR_NS::SdfPath PrimPickerDialog::selectedPath() const {
    const QModelIndexList rows = m_tree->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) return {};
    return PXR_NS::SdfPath(rows.front().data(kPathRole).toString().toStdString());
}

void PrimPickerDialog::setFilter(const QString& text) {
    if (m_filter->text() != text) m_filter->setText(text);  // calls back here
    m_proxy->setFilterFixedString(text);
    // Every match shows; without a filter, the tree as it was opened.
    if (!text.isEmpty()) m_tree->expandAll();
    const PXR_NS::SdfPath selected = selectedPath();
    if (!selected.IsEmpty()) m_tree->scrollTo(m_tree->selectionModel()->selectedRows(0).front());
}

void PrimPickerDialog::selectPath(const PXR_NS::SdfPath& path) {
    if (path.IsEmpty()) return;
    const QModelIndexList found =
        m_model->match(m_model->index(0, 0), kPathRole, qs(path.GetString()), 1, Qt::MatchExactly | Qt::MatchRecursive);
    if (found.isEmpty()) return;
    const QModelIndex index = m_proxy->mapFromSource(found.front());
    if (!index.isValid()) return;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) m_tree->expand(parent);
    m_tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_tree->scrollTo(index);
}

int PrimPickerDialog::visiblePrimCount() const {
    int count = 0;
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            ++count;
            pending.push_back(m_proxy->index(row, 0, parent));
        }
    }
    return count;
}

std::optional<PXR_NS::SdfPath> pickPrimFromFile(const QString& file, const PXR_NS::SdfPath& current, QWidget* parent) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(file.toStdString(), PXR_NS::UsdStage::LoadNone);
    QApplication::restoreOverrideCursor();
    if (!stage) {
        QMessageBox::warning(parent, QObject::tr("Select Prim"), QObject::tr("Can't open %1.").arg(file));
        return std::nullopt;
    }
    PrimPickerDialog dialog(stage, current, parent);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedPath().IsEmpty()) return std::nullopt;
    return dialog.selectedPath();
}

}  // namespace usdcc::usd
