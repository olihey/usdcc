#include "usdcc/usd/properties/transform_stack_editor.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/attribute_binding.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/xform_math.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>

namespace usdcc::usd::properties {

using namespace PXR_NS;

namespace {

constexpr int kLabelWidth = 130;

// The rows' container. Reordering is a plain in-widget drag (a row only ever
// moves within this list, so there's no need for system drag and drop): the
// grip reports where it is, this shows a line where the row would land, and
// the release moves the op.
class StackRows : public QWidget {
public:
    StackRows(TransformStackEditor* editor, QWidget* parent) : QWidget(parent), m_editor(editor) {
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        usdcc::ui::scaled::spacing(m_layout, 6);
        m_indicator = new QFrame(this);
        m_indicator->setObjectName(QStringLiteral("dropIndicator"));
        usdcc::ui::scaled::fixedHeight(m_indicator, 2);
        m_indicator->hide();
    }
    void addRow(QWidget* row) { m_layout->addWidget(row); }

    void dragTo(int y) {
        const int slot = slotAt(y);
        int lineY = 0;
        if (slot < m_layout->count()) {
            lineY = m_layout->itemAt(slot)->geometry().top() - usdcc::ui::scaled::pixels(4);
        } else if (m_layout->count() > 0) {
            lineY = m_layout->itemAt(m_layout->count() - 1)->geometry().bottom() + usdcc::ui::scaled::pixels(2);
        }
        m_indicator->setGeometry(0, std::max(0, lineY), width(), usdcc::ui::scaled::pixels(2));
        m_indicator->show();
        m_indicator->raise();
    }
    void dropAt(size_t from, int y) {
        m_indicator->hide();
        const size_t slot = static_cast<size_t>(slotAt(y));
        const size_t to = slot > from ? slot - 1 : slot;
        if (to != from) m_editor->moveOp(from, to);
    }

private:
    // How many rows lie above `y`: the index a row dropped there ends up at.
    int slotAt(int y) const {
        int slot = 0;
        for (int i = 0; i < m_layout->count(); ++i) {
            if (y > m_layout->itemAt(i)->geometry().center().y()) slot = i + 1;
        }
        return slot;
    }

    TransformStackEditor* m_editor;
    QVBoxLayout* m_layout;
    QFrame* m_indicator;
};

// The "≡" grip a row is dragged by.
class DragHandle : public QLabel {
public:
    DragHandle(size_t index, StackRows* rows, QWidget* parent)
        : QLabel(QStringLiteral("\xE2\x89\xA1"), parent), m_index(index), m_rows(rows) {
        setObjectName(QStringLiteral("dragHandle"));
        setCursor(Qt::OpenHandCursor);
        setToolTip(tr("Drag to reorder"));
        usdcc::ui::scaled::fixedSize(this, 14, 20);
        setAlignment(Qt::AlignCenter);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        m_pressPos = event->pos();
        m_dragging = false;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton)) return;
        if (!m_dragging && (event->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        if (!m_dragging) {
            m_dragging = true;
            setCursor(Qt::ClosedHandCursor);
        }
        m_rows->dragTo(mapTo(m_rows, event->pos()).y());
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !m_dragging) return;
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        // Last: the move rebuilds the section, deleting this handle (deferred).
        m_rows->dropAt(m_index, mapTo(m_rows, event->pos()).y());
    }

private:
    size_t m_index;
    StackRows* m_rows;
    QPoint m_pressPos;
    bool m_dragging = false;
};

struct AddEntry {
    UsdGeomXformOp::Type type;
    const char* label;
};
const AddEntry kAddEntries[] = {
    {UsdGeomXformOp::TypeTranslate, "Translate"}, {UsdGeomXformOp::TypeRotateXYZ, "Rotate XYZ"},
    {UsdGeomXformOp::TypeRotateXZY, "Rotate XZY"}, {UsdGeomXformOp::TypeRotateYXZ, "Rotate YXZ"},
    {UsdGeomXformOp::TypeRotateYZX, "Rotate YZX"}, {UsdGeomXformOp::TypeRotateZXY, "Rotate ZXY"},
    {UsdGeomXformOp::TypeRotateZYX, "Rotate ZYX"}, {UsdGeomXformOp::TypeRotateX, "Rotate X"},
    {UsdGeomXformOp::TypeRotateY, "Rotate Y"},     {UsdGeomXformOp::TypeRotateZ, "Rotate Z"},
    {UsdGeomXformOp::TypeScale, "Scale"},          {UsdGeomXformOp::TypeOrient, "Orient (quaternion)"},
    {UsdGeomXformOp::TypeTransform, "Transform (matrix)"},
};

}  // namespace

TransformStackEditor::TransformStackEditor(CategorySection* section, const UsdPrim& prim, StageRefPtr stage,
                                           UndoManager* undoManager, QObject* parent)
    : QObject(parent), m_primPath(prim.GetPath()), m_stage(std::move(stage)), m_undoManager(undoManager) {
    const XformStack stack(prim);

    // Header: Reset Stack and + (Add).
    m_resetStack = new QCheckBox(tr("Reset Stack"));
    m_resetStack->setObjectName(QStringLiteral("sectionHeaderCheck"));
    m_resetStack->setToolTip(tr("Ignore the parent's transform (resetXformStack)"));
    m_resetStack->setChecked(stack.resetsXformStack());
    connect(m_resetStack, &QCheckBox::clicked, this, [this](bool on) {
        edit(on ? tr("Reset Xform Stack") : tr("Inherit Parent Transform"),
             [on](XformStack& s) { return s.setResetsXformStack(on); });
    });
    QToolButton* add = iconButton(QStringLiteral(":/usdcc/theme/plus.png"), tr("Add an op to the stack"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add);
    for (const AddEntry& entry : kAddEntries) {
        if (entry.type == UsdGeomXformOp::TypeRotateXYZ || entry.type == UsdGeomXformOp::TypeRotateX ||
            entry.type == UsdGeomXformOp::TypeScale) {
            addMenu->addSeparator();
        }
        const int type = static_cast<int>(entry.type);
        addMenu->addAction(tr(entry.label), this, [this, type]() { addOp(type); });
    }
    addMenu->addSeparator();
    populateStackActions(addMenu);
    add->setMenu(addMenu);
    section->addHeaderWidget(m_resetStack);
    section->addHeaderWidget(add);

    // One row per op.
    auto* rows = new StackRows(this, nullptr);
    const std::vector<UsdGeomXformOp> ops = stack.ops();
    for (size_t i = 0; i < ops.size(); ++i) {
        const UsdGeomXformOp& op = ops[i];
        OpRow entry;
        entry.opName = op.GetOpName();
        entry.attrName = op.GetAttr().GetName();
        const QString label = QString::fromStdString(XformStack::opLabel(op));
        PropertyEditor* editor = nullptr;
        if (op.IsInverseOp()) {
            editor = createPropertyEditor(SdfValueTypeNames->String);
            editor->setReadOnly(true);
            editor->setValue(VtValue(tr("Undoes %1").arg(QString::fromStdString(XformStack::opLabel(UsdGeomXformOp(op.GetAttr())))).toStdString()));
        } else {
            EditorHints hints = EditorHints::forAttribute(op.GetAttr());
            if (isRotation(op.GetOpType()) && op.GetOpType() != UsdGeomXformOp::TypeOrient) {
                hints.suffix = QStringLiteral("\xC2\xB0");
            }
            editor = createPropertyEditor(op.GetAttr().GetTypeName(), hints);
        }
        entry.row = new PropertyRow(label, editor);
        entry.row->setLabelWidth(kLabelWidth - 18);
        entry.row->addLeadingWidget(new DragHandle(i, rows, entry.row));
        const char* precision = op.GetPrecision() == UsdGeomXformOp::PrecisionDouble  ? "double"
                                : op.GetPrecision() == UsdGeomXformOp::PrecisionHalf ? "half"
                                                                                     : "float";
        entry.row->setLabelToolTip(tr("<b>%1</b><br>%2 precision<br><br>%3")
                                       .arg(QString::fromStdString(entry.opName.GetString()).toHtmlEscaped(),
                                            QString::fromLatin1(precision),
                                            op.IsInverseOp() ? tr("The inverse of its op; it has no value of its own.")
                                                             : tr("Click to make this the op the Move, Rotate and Scale "
                                                                  "tools act on; click again for Auto.")));
        entry.row->setPropertyPath(QString::fromStdString(op.GetAttr().GetPath().GetString()));
        if (!op.IsInverseOp()) {
            entry.binding = new AttributeBinding(op.GetAttr(), editor, m_stage, m_undoManager, entry.row);
            connect(entry.row, &PropertyRow::labelClicked, this, [this, i]() { toggleTarget(i); });
        }
        QToolButton* remove =
            iconButton(QStringLiteral(":/usdcc/theme/close.png"), tr("Remove this op from the stack"), entry.row);
        connect(remove, &QToolButton::clicked, this, [this, i, label]() {
            edit(tr("Remove %1").arg(label), [i](XformStack& s) { return s.remove(i); });
        });
        entry.row->addTrailingWidget(remove);
        entry.row->setExtraMenu([this, i](QMenu* menu) { populateRowMenu(menu, i); });
        rows->addRow(entry.row);
        m_rows.push_back(entry);
    }
    if (ops.empty()) {
        auto* empty = new QLabel(tr("No transform ops. Use + Add, or a Move/Rotate/Scale drag, to create one."));
        empty->setObjectName(QStringLiteral("propertyNote"));
        rows->addRow(empty);
    }
    section->addRow(rows);

    m_targetConnection = m_stage->targetXformOpChanged.connect([this]() { updateTargetMarkers(); });
    updateTargetMarkers();
}

UsdPrim TransformStackEditor::prim() const { return m_stage->usdStage()->GetPrimAtPath(m_primPath); }

bool TransformStackEditor::edit(const QString& description, const std::function<bool(XformStack&)>& change) {
    const UsdPrim p = prim();
    if (!p) return false;
    return m_undoManager->apply(m_stage, description, [&]() {
        XformStack stack(p);
        return change(stack);
    });
}

void TransformStackEditor::refresh() {
    const XformStack stack(prim());
    m_resetStack->setChecked(stack.resetsXformStack());
    updateTargetMarkers();
}

void TransformStackEditor::moveOp(size_t from, size_t to) {
    edit(tr("Reorder Transform Ops"), [from, to](XformStack& s) { return s.move(from, to); });
}

size_t TransformStackEditor::insertIndex() const {
    // After the targeted op, else at the end.
    const TfToken target = m_stage->targetXformOpFor(m_primPath);
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (!target.IsEmpty() && m_rows[i].opName == target) return i + 1;
    }
    return m_rows.size();
}

void TransformStackEditor::addOp(int type) {
    const auto opType = static_cast<UsdGeomXformOp::Type>(type);
    const QString name = QString::fromStdString(UsdGeomXformOp::GetOpTypeToken(opType).GetString());
    const size_t index = insertIndex();
    edit(tr("Add %1").arg(name), [opType, index](XformStack& s) { return s.add(opType, index).has_value(); });
}

void TransformStackEditor::toggleTarget(size_t index) {
    if (index >= m_rows.size()) return;
    const TfToken opName = m_rows[index].opName;
    if (m_stage->targetXformOpFor(m_primPath) == opName) {
        m_stage->clearTargetXformOp();
    } else {
        m_stage->setTargetXformOp(m_primPath, opName);
    }
}

void TransformStackEditor::updateTargetMarkers() {
    const TfToken target = m_stage->targetXformOpFor(m_primPath);
    for (const OpRow& row : m_rows) {
        row.row->setTargeted(!target.IsEmpty() && row.opName == target);
    }
}

void TransformStackEditor::populateRowMenu(QMenu* menu, size_t index) {
    const bool inverse = index < m_rows.size() && !m_rows[index].binding;
    QAction* up = menu->addAction(tr("Move Up"), this, [this, index]() { moveOp(index, index - 1); });
    up->setEnabled(index > 0);
    QAction* down = menu->addAction(tr("Move Down"), this, [this, index]() { moveOp(index, index + 1); });
    down->setEnabled(index + 1 < m_rows.size());
    menu->addSeparator();
    QAction* target = menu->addAction(tr("Use as Tool Target"), this, [this, index]() { toggleTarget(index); });
    target->setCheckable(true);
    target->setChecked(index < m_rows.size() && m_stage->targetXformOpFor(m_primPath) == m_rows[index].opName);
    target->setEnabled(!inverse);
    QAction* rename = menu->addAction(tr("Rename Suffix\xE2\x80\xA6"), this, [this, index]() {
        bool ok = false;
        const QString suffix = QInputDialog::getText(nullptr, tr("Rename Op"), tr("New suffix:"), QLineEdit::Normal,
                                                     QString(), &ok)
                                   .trimmed();
        if (ok && !suffix.isEmpty()) {
            edit(tr("Rename Op"), [index, suffix](XformStack& s) { return s.rename(index, TfToken(suffix.toStdString())); });
        }
    });
    rename->setEnabled(!inverse);
    QAction* reset = menu->addAction(tr("Reset to Identity"), this, [this, index]() {
        edit(tr("Reset Op"), [index](XformStack& s) { return s.resetToIdentity(index); });
    });
    reset->setEnabled(!inverse);
    QAction* pivot = menu->addAction(tr("Add Pivot Around"), this, [this, index]() {
        edit(tr("Add Pivot"), [index](XformStack& s) { return s.addPivot(index); });
    });
    pivot->setEnabled(!inverse);
    menu->addAction(tr("Remove"), this, [this, index]() {
        edit(tr("Remove Op"), [index](XformStack& s) { return s.remove(index); });
    });
    menu->addSeparator();
    populateStackActions(menu);
}

void TransformStackEditor::populateStackActions(QMenu* menu) {
    QAction* bake = menu->addAction(tr("Bake Stack to Matrix"), this, [this]() {
        edit(tr("Bake Transform"), [](XformStack& s) { return s.bakeToMatrix(); });
    });
    QAction* convert = menu->addAction(tr("Convert to Translate / Rotate / Scale"), this, [this]() {
        edit(tr("Convert Transform"), [](XformStack& s) { return s.convertToTRS(); });
    });
    // Availability follows the stack as it is when the menu opens (the + Add
    // menu lives as long as the section; a row's menu is built on demand).
    auto update = [this, bake, convert]() {
        const XformStack stack(prim());
        bake->setEnabled(!stack.ops().empty());
        convert->setEnabled(stack.canConvertToTRS() && !stack.ops().empty());
    };
    connect(menu, &QMenu::aboutToShow, this, update);
    update();
}

}  // namespace usdcc::usd::properties
