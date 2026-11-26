#include "usdcc/usd/primvar_values_view.h"

#include "usdcc/ui/theme.h"
#include "usdcc/usd/value_type_info.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QHeaderView>
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

    if (constant) {
        m_rows = 1;
        m_constant = true;
        m_domain = tr("constant: one value for the prim");
    } else if (expected.count) {
        m_rows = *expected.count;
        m_domain = counted(m_rows, expected.of);
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

PrimvarValuesView::PrimvarValuesView(QWidget* parent)
    : PrimvarView(parent), m_status(new QLabel(this)), m_table(new QTableView(this)), m_model(new PrimvarValuesModel(this)) {
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
        }
        menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_status);
    layout->addWidget(m_table, 1);
}

void PrimvarValuesView::setContext(const PrimvarViewContext& context, bool focusOnly) {
    const PrimvarSet* set = context.setWith();
    const std::string shown = set ? set->prim.GetString() + '\n' + context.primvar : std::string();
    if (focusOnly && shown == m_shown) return;
    const bool same = shown == m_shown && !shown.empty();
    const int scroll = m_table->verticalScrollBar()->value();
    QString why;
    const bool ok = m_model->setContext(context, &why);
    m_status->setText(ok ? m_model->domain() : why);
    m_table->setVisible(ok);
    if (ok) {
        m_table->resizeColumnsToContents();
        if (same) m_table->verticalScrollBar()->setValue(scroll);
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
