#include "usdcc/usd/primvar_values_view.h"

#include "usdcc/ui/theme.h"
#include "usdcc/usd/primvar_commands.h"
#include "usdcc/usd/value_type_info.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPointer>
#include <QStyle>
#include <QFont>
#include <QHeaderView>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <set>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }

// "1 point", "6 points".
QString counted(std::size_t count, const std::string& plural) {
    QString unit = qs(plural);
    if (count == 1 && unit.endsWith(QLatin1Char('s'))) unit.chop(1);
    return QStringLiteral("%1 %2").arg(count).arg(unit);
}

// How a value reads in a cell: numbers as the Attribute Editor has them, a whole array for one
// that's constant.
QString format(const VtValue& value) { return qs(formatValue(value, 4, 16)); }

// The values of a primvar, at a time, as authored on `prim`.
bool read(const UsdPrim& prim, const std::string& name, UsdTimeCode time, PrimvarValuesModel::Column& column) {
    const UsdAttribute values = prim.GetAttribute(TfToken("primvars:" + name));
    if (!values || !values.Get(&column.values, time)) return false;
    if (column.indexed) prim.GetAttribute(TfToken("primvars:" + name + ":indices")).Get(&column.indices, time);
    return true;
}
}  // namespace

PrimvarValuesModel::PrimvarValuesModel(QObject* parent) : QAbstractTableModel(parent) {}

bool PrimvarValuesModel::setContext(const PrimvarViewContext& context, QString* why) {
    beginResetModel();
    m_columns.clear();
    m_rows = 0;
    m_constant = false;
    m_domain.clear();
    m_faceStarts.clear();
    m_faceVertexIndices = {};
    m_prim = {};
    m_elementDomain.clear();
    const auto fail = [&](const QString& reason) {
        if (why) *why = reason;
        endResetModel();
        return false;
    };
    const PrimvarSet* set = context.setWith();
    const PrimvarInfo* focus = context.info();
    if (context.prims.empty()) return fail(tr("Select a prim to see its primvars' values."));
    if (!focus || !set || !context.stage) return fail(context.primvar.empty() ? tr("Pick a primvar in the list.")
                                                                              : tr("None of the selected prims has it."));
    if (!focus->hasValue) return fail(tr("It has no values: it's declared only, or blocked."));
    const UsdPrim prim = context.stage->usdStage()->GetPrimAtPath(set->prim);
    if (!prim) return fail(tr("The prim is gone."));

    const TfToken& interpolation = focus->interpolation;
    const bool constant = interpolation == UsdGeomTokens->constant;
    // The focus first, then the others of its interpolation, by name. Inherited ones are
    // constant, so they share only the constant domain.
    std::vector<const PrimvarInfo*> shown{focus};
    for (const PrimvarInfo& info : set->primvars) {
        if (&info == focus || !info.hasValue || info.interpolation != interpolation) continue;
        if (info.source != PrimvarSource::Local && !constant) continue;
        shown.push_back(&info);
    }
    for (const PrimvarInfo* info : shown) {
        Column column;
        column.name = qs(info->name);
        column.indexed = info->indexed;
        column.elementSize = std::max(info->elementSize, 1);
        column.unauthoredValuesIndex = info->unauthoredValuesIndex;
        column.editable = info->sourcePrim == set->prim;
        const UsdPrim source = context.stage->usdStage()->GetPrimAtPath(info->sourcePrim);
        if (source && read(source, info->name, context.time, column)) m_columns.push_back(std::move(column));
    }
    if (m_columns.empty()) return fail(tr("Its values can't be read."));

    const PrimvarElementCounts counts = primvarElementCounts(prim, context.time);
    const PrimvarElementCounts::Count expected = counts.of(interpolation);
    // Where the elements are, on a mesh: its points, or a corner's face and point.
    if (!constant && (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying) &&
        expected.of == "points") {
        Column points;
        points.kind = Column::Kind::Points;
        points.name = tr("points");
        if (UsdGeomPointBased(prim).GetPointsAttr().Get(&points.values, context.time)) m_columns.push_back(std::move(points));
    }
    if (interpolation == UsdGeomTokens->faceVarying && prim.IsA<UsdGeomMesh>()) {
        const UsdGeomMesh mesh(prim);
        VtIntArray faces;
        if (mesh.GetFaceVertexCountsAttr().Get(&faces, context.time) && mesh.GetFaceVertexIndicesAttr().Get(&m_faceVertexIndices, context.time)) {
            m_faceStarts.reserve(faces.size());
            std::size_t start = 0;
            for (const int face : faces) {
                m_faceStarts.push_back(start);
                start += std::size_t(std::max(face, 0));
            }
            Column face;
            face.kind = Column::Kind::Face;
            face.name = tr("face");
            Column point;
            point.kind = Column::Kind::Point;
            point.name = tr("point");
            m_columns.push_back(std::move(face));
            m_columns.push_back(std::move(point));
        }
    }

    m_prim = set->prim;
    if (constant) {
        m_rows = 1;
        m_constant = true;
        m_domain = tr("constant: one value for the prim");
    } else if (expected.count) {
        m_rows = *expected.count;
        m_domain = counted(m_rows, expected.of);
        m_elementDomain = expected.of;
    } else {
        // USD doesn't say how many: as many as the longest has.
        for (const Column& column : m_columns) {
            if (column.kind != Column::Kind::Primvar) continue;
            m_rows = std::max(m_rows, column.indexed ? column.indices.size() : arraySize(column.values) / std::size_t(column.elementSize));
        }
        m_domain = counted(m_rows, "elements");
    }
    m_domain += tr(", %1 on %2").arg(qs(interpolation.GetString()), qs(set->prim.GetString()));
    endResetModel();
    return true;
}

std::size_t PrimvarValuesModel::faceOf(std::size_t corner) const {
    const auto after = std::upper_bound(m_faceStarts.begin(), m_faceStarts.end(), corner);
    return after == m_faceStarts.begin() ? 0 : std::size_t(after - m_faceStarts.begin()) - 1;
}

QString PrimvarValuesModel::cell(const Column& column, std::size_t row, bool* problem) const {
    const auto bad = [problem](const QString& text) {
        if (problem) *problem = true;
        return text;
    };
    switch (column.kind) {
        case Column::Kind::Points: return row < arraySize(column.values) ? format(arrayElement(column.values, row)) : bad(tr("none"));
        case Column::Kind::Face: return m_faceStarts.empty() ? QString() : QString::number(faceOf(row));
        case Column::Kind::Point: return row < m_faceVertexIndices.size() ? QString::number(m_faceVertexIndices[row]) : bad(tr("none"));
        case Column::Kind::Primvar: break;
    }
    if (m_constant) return format(column.values);
    if (!column.values.IsArrayValued()) return row == 0 ? format(column.values) : bad(tr("none"));
    const std::size_t size = std::size_t(column.elementSize);
    const std::size_t count = arraySize(column.values);
    std::size_t element = row;
    QString index;
    if (column.indexed) {
        if (row >= column.indices.size()) return bad(tr("no index"));
        const int at = column.indices[row];
        index = QStringLiteral("  ⟨%1⟩").arg(at);
        if (column.unauthoredValuesIndex >= 0 && at == column.unauthoredValuesIndex) return tr("no value") + index;
        if (at < 0 || (std::size_t(at) + 1) * size > count) return bad(tr("outside the values") + index);
        element = std::size_t(at);
    } else if ((row + 1) * size > count) {
        return bad(tr("none"));
    }
    QString text;
    if (size == 1) {
        text = format(arrayElement(column.values, element));
    } else {
        QStringList parts;
        for (std::size_t i = 0; i < size; ++i) parts << format(arrayElement(column.values, element * size + i));
        text = QStringLiteral("[%1]").arg(parts.join(QStringLiteral(", ")));
    }
    return text + index;
}

int PrimvarValuesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(std::min<std::size_t>(m_rows, std::size_t(std::numeric_limits<int>::max())));
}

int PrimvarValuesModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_columns.size()); }

QVariant PrimvarValuesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.column() >= int(m_columns.size())) return {};
    const Column& column = m_columns[std::size_t(index.column())];
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return cell(column, std::size_t(index.row()));
    if (role == Qt::EditRole) return editText(column, std::size_t(index.row()));
    if (role == Qt::ForegroundRole) {
        bool problem = false;
        cell(column, std::size_t(index.row()), &problem);
        if (problem) return usdcc::ui::theme::color(usdcc::ui::theme::Token::Error);
        if (column.kind != Column::Kind::Primvar) return usdcc::ui::theme::color(usdcc::ui::theme::Token::Muted);
    }
    return {};
}

QVariant PrimvarValuesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Vertical) return role == Qt::DisplayRole ? QVariant(section) : QVariant();
    if (section < 0 || section >= int(m_columns.size())) return {};
    const Column& column = m_columns[std::size_t(section)];
    if (role == Qt::DisplayRole) return column.name;
    if (role == Qt::FontRole && section == 0) {
        QFont bold;
        bold.setBold(true);
        return bold;
    }
    if (role == Qt::ToolTipRole && column.kind == Column::Kind::Primvar) {
        QString tip = QStringLiteral("primvars:") + column.name;
        if (column.indexed) tip += tr("\nIndexed: the value each index picks, and the index in ⟨ ⟩");
        if (column.elementSize > 1) tip += tr("\n%1 values an element").arg(column.elementSize);
        return tip;
    }
    return {};
}

Qt::ItemFlags PrimvarValuesModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() < int(m_columns.size())) {
        const Column& column = m_columns[std::size_t(index.column())];
        if (column.kind == Column::Kind::Primvar && column.editable && m_editor) flags |= Qt::ItemIsEditable;
    }
    return flags;
}

bool PrimvarValuesModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable)) return false;
    // The view writes it; the stage's change brings it back here.
    return m_editor(index.column(), std::size_t(index.row()), value.toString());
}

QString PrimvarValuesModel::editText(const Column& column, std::size_t row) const {
    // One value, every digit; a string quoted where a comma or quote in it would split it.
    const auto full = [](const VtValue& value) {
        const QString text = qs(formatValue(value, 9, std::numeric_limits<std::size_t>::max()));
        const bool words = value.IsHolding<std::string>() || value.IsHolding<TfToken>() || value.IsHolding<SdfAssetPath>();
        if (!words || (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')))) return text;
        QString quoted = text;
        quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return QStringLiteral("\"%1\"").arg(quoted);
    };
    if (!column.values.IsArrayValued()) return full(column.values);
    if (m_constant) {
        // The whole array's values, without its length in front ("[3] a, b, c" is what the cell shows).
        QStringList parts;
        for (std::size_t i = 0; i < arraySize(column.values); ++i) parts << full(arrayElement(column.values, i));
        return parts.join(QStringLiteral(", "));
    }
    const std::size_t size = std::size_t(column.elementSize);
    std::size_t element = row;
    if (column.indexed) {
        if (row >= column.indices.size() || column.indices[row] < 0) return {};
        element = std::size_t(column.indices[row]);
    }
    if ((element + 1) * size > arraySize(column.values)) return {};
    if (size == 1) return full(arrayElement(column.values, element));
    QStringList parts;
    for (std::size_t i = 0; i < size; ++i) parts << full(arrayElement(column.values, element * size + i));
    return QStringLiteral("[%1]").arg(parts.join(QStringLiteral(", ")));
}

QString PrimvarValuesModel::text(const QModelIndexList& indexes) const {
    std::set<int> rows, columns;
    for (const QModelIndex& index : indexes) {
        rows.insert(index.row());
        columns.insert(index.column());
    }
    QStringList lines;
    QStringList header;
    for (const int column : columns) header << m_columns[std::size_t(column)].name;
    lines << header.join(QLatin1Char('\t'));
    std::set<std::pair<int, int>> chosen;
    for (const QModelIndex& index : indexes) chosen.insert({index.row(), index.column()});
    for (const int row : rows) {
        QStringList cells;
        for (const int column : columns) {
            cells << (chosen.count({row, column}) ? cell(m_columns[std::size_t(column)], std::size_t(row)) : QString());
        }
        lines << cells.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n'));
}

// --- PrimvarValuesView ---------------------------------------------------------------

namespace {

void showReport(QLabel* label, const QString& what, const PrimvarEdit& edit) {
    if (!label) return;
    QStringList notes;
    for (const std::string& note : edit.notes) notes << QString::fromStdString(note);
    label->setText(edit.ok ? (notes.isEmpty() ? QObject::tr("%1: done.").arg(what) : QObject::tr("%1: done. %2").arg(what, notes.join(QLatin1Char(' '))))
                           : QObject::tr("%1 wasn't done: %2").arg(what, QString::fromStdString(edit.error)));
    label->setProperty("severity", edit.ok ? (notes.isEmpty() ? "info" : "warning") : "error");
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->show();
}

// What each number of an element is called: x, y, z; r, g, b; u, v; or 1, 2, 3 for more.
QStringList componentNames(const UsdPrim& prim, const std::string& name, std::size_t numbers) {
    QStringList names;
    if (numbers <= 1) return names;
    const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken(name));
    const SdfValueTypeName type = primvar ? primvar.GetTypeName() : SdfValueTypeName();
    const SdfTupleDimensions dimensions = type ? type.GetDimensions() : SdfTupleDimensions();
    if (dimensions.size == 1 && dimensions.d[0] == numbers && numbers <= 4) {
        const TfToken& role = type.GetRole();
        const char* const letters = role == SdfValueRoleNames->Color ? "rgba" : role == SdfValueRoleNames->TextureCoordinate ? "uvw" : "xyzw";
        for (std::size_t i = 0; i < numbers && letters[i]; ++i) names << QString(QLatin1Char(letters[i]));
        if (std::size_t(names.size()) == numbers) return names;
        names.clear();
    }
    for (std::size_t i = 0; i < numbers; ++i) names << QString::number(i + 1);
    return names;
}

QString kindName(PrimvarValueChange::Kind kind) {
    switch (kind) {
        case PrimvarValueChange::Kind::Set: return QObject::tr("Set");
        case PrimvarValueChange::Kind::Add: return QObject::tr("Add");
        case PrimvarValueChange::Kind::Multiply: return QObject::tr("Multiply");
        case PrimvarValueChange::Kind::Clamp: return QObject::tr("Clamp");
        case PrimvarValueChange::Kind::Remap: return QObject::tr("Remap");
    }
    return {};
}

// The dialog Set, Add, Multiply, Clamp and Remap ask with: which number of each element, and the
// numbers the change takes.
std::optional<PrimvarValueChange> askChangeDialog(QWidget* parent, PrimvarValueChange::Kind kind, const QString& primvar,
                                                  const QStringList& components, bool text) {
    using Kind = PrimvarValueChange::Kind;
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("%1 %2").arg(kindName(kind), primvar));
    auto* form = new QFormLayout(&dialog);
    QComboBox* component = nullptr;
    if (!components.isEmpty()) {
        component = new QComboBox(&dialog);
        component->addItem(QObject::tr("All"));
        component->addItems(components);
        form->addRow(QObject::tr("Of each:"), component);
    }
    QStringList labels;
    switch (kind) {
        case Kind::Set: labels << (text ? QObject::tr("Text:") : QObject::tr("Value:")); break;
        case Kind::Add: labels << QObject::tr("Add:"); break;
        case Kind::Multiply: labels << QObject::tr("Multiply by:"); break;
        case Kind::Clamp: labels << QObject::tr("Low:") << QObject::tr("High:"); break;
        case Kind::Remap: labels << QObject::tr("From low:") << QObject::tr("From high:") << QObject::tr("To low:") << QObject::tr("To high:"); break;
    }
    std::vector<QLineEdit*> fields;
    for (const QString& label : labels) {
        auto* field = new QLineEdit(&dialog);
        if (kind == Kind::Multiply) field->setText(QStringLiteral("1"));
        if (kind == Kind::Remap) field->setText(fields.size() % 2 ? QStringLiteral("1") : QStringLiteral("0"));
        form->addRow(label, field);
        fields.push_back(field);
    }
    if (kind == Kind::Set && !text && components.size() > 1) {
        auto* hint = new QLabel(QObject::tr("One number for all of them, or %1 for each element's.").arg(components.size()), &dialog);
        hint->setWordWrap(true);
        form->addRow(hint);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    PrimvarValueChange change;
    change.kind = kind;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        change.numbers.clear();
        change.component = component ? component->currentIndex() - 1 : -1;
        if (text) {
            change.text = splitTexts(fields.front()->text().toStdString());
            dialog.accept();
            return;
        }
        for (QLineEdit* field : fields) {
            const std::optional<std::vector<double>> numbers = parseNumbers(field->text().toStdString());
            const bool many = kind == Kind::Set || kind == Kind::Add || kind == Kind::Multiply;
            if (!numbers || numbers->empty() || (!many && numbers->size() != 1)) {
                field->setFocus();
                field->selectAll();
                return;  // stays open
            }
            change.numbers.insert(change.numbers.end(), numbers->begin(), numbers->end());
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return change;
}

}  // namespace

std::vector<std::size_t> PrimvarValuesView::pickedRows() const {
    std::set<std::size_t> rows;
    for (const QModelIndex& index : m_table->selectionModel()->selectedIndexes()) rows.insert(std::size_t(index.row()));
    if (rows.empty() && m_table->currentIndex().isValid()) rows.insert(std::size_t(m_table->currentIndex().row()));
    return {rows.begin(), rows.end()};
}

QString PrimvarValuesView::lastReport() const { return m_report->isHidden() ? QString() : m_report->text(); }

bool PrimvarValuesView::editCell(int column, std::size_t row, const QString& text) {
    if (!m_stage || column < 0 || column >= m_model->columnCount()) return false;
    const PrimvarValuesModel::Column& shown = m_model->columns()[std::size_t(column)];
    if (shown.kind != PrimvarValuesModel::Column::Kind::Primvar || !shown.editable) return false;
    const std::string name = shown.name.toStdString();
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(m_model->prim());
    PrimvarValueChange change;
    if (primvarElementNumbers(prim, name) == 0) {
        // One text for each of the element's values (a constant string[]'s), or one for all.
        change.text = splitTexts(text.toStdString());
    } else {
        const std::optional<std::vector<double>> numbers = parseNumbers(text.toStdString());
        if (!numbers || numbers->empty()) {
            PrimvarEdit refused;
            refused.error = "\"" + text.toStdString() + "\" isn't numbers.";
            showReport(m_report, tr("Edit %1").arg(shown.name), refused);
            return false;
        }
        change.numbers = *numbers;
    }
    return changeValues(shown.name, {row}, change);
}

bool PrimvarValuesView::changeValues(const QString& primvar, std::vector<std::size_t> rows, const PrimvarValueChange& change) {
    if (!m_stage || m_model->prim().IsEmpty()) return false;
    PrimvarCommandContext context;
    context.undo = m_undo;
    context.stage = m_stage;
    context.prims = {m_model->prim()};
    context.primvar = primvar.toStdString();
    QPointer<QLabel> report(m_report);
    context.report = [report](const QString& what, const PrimvarEdit& edit) { showReport(report, what, edit); };
    QString what;
    switch (change.kind) {
        case PrimvarValueChange::Kind::Set: what = rows.size() == 1 ? tr("Edit %1") : tr("Set %1"); break;
        case PrimvarValueChange::Kind::Add: what = tr("Add to %1"); break;
        case PrimvarValueChange::Kind::Multiply: what = tr("Multiply %1"); break;
        case PrimvarValueChange::Kind::Clamp: what = tr("Clamp %1"); break;
        case PrimvarValueChange::Kind::Remap: what = tr("Remap %1"); break;
    }
    const std::string name = context.primvar;
    return runPrimvarEdit(context, what.arg(primvar), context.prims,
                          [&](const UsdPrim& prim) { return changePrimvarValues(prim, name, rows, change); })
        .ok;
}

void PrimvarValuesView::addChangeMenu(QMenu* menu, int column) {
    if (!m_stage || column < 0 || column >= m_model->columnCount()) return;
    const PrimvarValuesModel::Column& shown = m_model->columns()[std::size_t(column)];
    if (shown.kind != PrimvarValuesModel::Column::Kind::Primvar || !shown.editable) return;
    const QString name = shown.name;
    const UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(m_model->prim());
    const std::size_t numbers = primvarElementNumbers(prim, name.toStdString());
    const QStringList components = componentNames(prim, name.toStdString(), numbers);
    const bool text = numbers == 0;
    const std::size_t rows = pickedRows().size();
    QMenu* change = menu->addMenu(tr("Change %1").arg(name));
    change->setObjectName(QStringLiteral("primvarValuesChange"));
    change->setToolTipsVisible(true);
    using Kind = PrimvarValueChange::Kind;
    for (const Kind kind : {Kind::Set, Kind::Add, Kind::Multiply, Kind::Clamp, Kind::Remap}) {
        if (text && kind != Kind::Set) continue;
        QAction* action = change->addAction(kindName(kind) + QStringLiteral("..."));
        action->setObjectName(QStringLiteral("primvarValues.") + kindName(kind).toLower());
        if (rows == 0) {
            action->setEnabled(false);
            action->setToolTip(tr("Pick rows first"));
        }
        connect(action, &QAction::triggered, this, [this, kind, name, components, text]() {
            const std::vector<std::size_t> picked = pickedRows();
            if (picked.empty()) return;
            const std::optional<PrimvarValueChange> asked =
                m_askChange ? m_askChange(kind, name, components, text) : askChangeDialog(this, kind, name, components, text);
            if (asked) changeValues(name, picked, *asked);
        });
    }
}

PrimvarValuesView::PrimvarValuesView(QWidget* parent)
    : PrimvarView(parent),
      m_status(new QLabel(this)),
      m_report(new QLabel(this)),
      m_table(new QTableView(this)),
      m_model(new PrimvarValuesModel(this)) {
    m_status->setObjectName(QStringLiteral("primvarValuesStatus"));
    m_status->setContentsMargins(6, 4, 6, 4);
    m_status->setWordWrap(true);
    m_table->setObjectName(QStringLiteral("primvarValues"));
    m_table->setModel(m_model);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setWordWrap(false);
    m_table->setCornerButtonEnabled(false);
    // Rows of one height: a million of them scroll without being measured.
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->horizontalHeader()->setResizeContentsPrecision(200);
    m_table->horizontalHeader()->setStretchLastSection(true);

    auto* copyAction = new QAction(tr("Copy"), m_table);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(copyAction, &QAction::triggered, this, &PrimvarValuesView::copy);
    m_table->addAction(copyAction);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this, [this, copyAction](const QPoint& pos) {
        QMenu menu(this);
        menu.addAction(copyAction);
        const int column = m_table->columnAt(pos.x());
        if (column >= 0) {
            menu.addAction(tr("Select Column"), this, [this, column]() { m_table->selectColumn(column); });
            addChangeMenu(&menu, column);
        }
        menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_status);
    // How the last edit went (docs/PRIMVAR_VIEWER.md §20).
    m_report->setObjectName(QStringLiteral("primvarsReport"));
    m_report->setContentsMargins(6, 0, 6, 4);
    m_report->setWordWrap(true);
    m_report->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_report->hide();
    layout->addWidget(m_report);
    layout->addWidget(m_table, 1);
    m_model->setCellEditor([this](int column, std::size_t row, const QString& text) { return editCell(column, row, text); });

    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &PrimvarValuesView::rowsPicked);
    // The row under the cursor is the stage's hover.
    m_table->setMouseTracking(true);
    m_table->viewport()->installEventFilter(this);
    connect(m_table, &QTableView::entered, this, [this](const QModelIndex& index) {
        if (m_stage && index.isValid()) m_stage->setPrimvarElementHover(elementsOf({std::size_t(index.row())}));
    });
}

PrimvarElements PrimvarValuesView::elementsOf(std::vector<std::size_t> rows) const {
    if (m_model->elementDomain().empty()) return {};
    return {m_model->prim(), m_model->elementDomain(), std::move(rows)};
}

bool PrimvarValuesView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_table->viewport() && event->type() == QEvent::Leave && m_stage &&
        m_stage->primvarElementHover().prim == m_model->prim()) {
        m_stage->setPrimvarElementHover({});
    }
    return PrimvarView::eventFilter(watched, event);
}

void PrimvarValuesView::rowsPicked() {
    if (m_syncing || !m_stage) return;
    std::vector<std::size_t> rows;
    for (const QModelIndex& index : m_table->selectionModel()->selectedIndexes()) rows.push_back(std::size_t(index.row()));
    m_syncing = true;
    m_stage->setPrimvarElements(elementsOf(std::move(rows)));
    m_syncing = false;
}

void PrimvarValuesView::showStageElements() {
    if (m_syncing || !m_stage) return;
    const PrimvarElements& elements = m_stage->primvarElements();
    const bool ours = !elements.empty() && elements.prim == m_model->prim() && elements.domain == m_model->elementDomain();
    m_syncing = true;
    QItemSelection selection;
    if (ours) {
        // Runs of rows, so that many in a row are one range.
        const int columns = m_model->columnCount();
        std::size_t start = 0;
        for (std::size_t i = 1; i <= elements.indices.size(); ++i) {
            if (i < elements.indices.size() && elements.indices[i] == elements.indices[i - 1] + 1) continue;
            const int first = int(elements.indices[start]), last = int(elements.indices[i - 1]);
            if (first < m_model->rowCount()) {
                selection.select(m_model->index(first, 0), m_model->index(std::min(last, m_model->rowCount() - 1), columns - 1));
            }
            start = i;
        }
    }
    m_table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    if (ours && !selection.isEmpty()) m_table->scrollTo(selection.first().topLeft());
    m_syncing = false;
}

void PrimvarValuesView::setContext(const PrimvarViewContext& context, bool focusOnly) {
    const PrimvarSet* set = context.setWith();
    const std::string shown = set ? set->prim.GetString() + '\n' + context.primvar : std::string();
    if (focusOnly && shown == m_shown) return;
    const bool same = shown == m_shown && !shown.empty();
    const int scroll = m_table->verticalScrollBar()->value();
    m_undo = context.undo;
    if (context.stage != m_stage) {
        m_stage = context.stage;
        m_elementsConnection = m_stage ? m_stage->primvarElementsChanged.connect([this]() { showStageElements(); })
                                       : usdcc::core::Signal<>::Connection();
    }
    QString why;
    m_syncing = true;  // the reset's empty selection isn't the user's
    const bool ok = m_model->setContext(context, &why);
    m_syncing = false;
    m_status->setText(ok ? m_model->domain() : why);
    m_table->setVisible(ok);
    if (ok) {
        m_table->resizeColumnsToContents();
        if (same) m_table->verticalScrollBar()->setValue(scroll);
        showStageElements();
    }
    m_shown = shown;
}

QString PrimvarValuesView::selectedText() const {
    QModelIndexList indexes = m_table->selectionModel()->selectedIndexes();
    if (indexes.isEmpty() && m_table->currentIndex().isValid()) indexes << m_table->currentIndex();
    return indexes.isEmpty() ? QString() : m_model->text(indexes);
}

void PrimvarValuesView::copy() const {
    const QString text = selectedText();
    if (!text.isEmpty()) QApplication::clipboard()->setText(text);
}

}  // namespace usdcc::usd
