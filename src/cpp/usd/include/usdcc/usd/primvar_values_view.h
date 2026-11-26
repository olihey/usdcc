#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/primvar_edit.h"
#include "usdcc/usd/primvar_views.h"

#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <QAbstractTableModel>
#include <QEvent>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

class QLabel;
class QMenu;
class QTableView;

namespace usdcc::usd {

// The elements of the focused primvar's domain (docs/PRIMVAR_VIEWER.md §8, Values): a row for
// each face, point or face corner, a column for each primvar of that interpolation on the prim,
// and for a mesh's points and corners where they are and which face. An indexed primvar shows
// the value its index picks, with the index. Virtual: only the rows on screen are formatted, so a
// mesh of millions of points opens at once.
class USDCC_USD_UI_API PrimvarValuesModel : public QAbstractTableModel {
    Q_OBJECT

public:
    struct Column {
        enum class Kind { Primvar, Points, Face, Point };
        Kind kind = Kind::Primvar;
        QString name;
        PXR_NS::VtValue values;
        PXR_NS::VtIntArray indices;
        bool indexed = false;
        int elementSize = 1;
        int unauthoredValuesIndex = -1;
        bool editable = false;  // the shown prim's own: its cells can be edited (§20)
    };

    explicit PrimvarValuesModel(QObject* parent = nullptr);

    // The domain of `context`'s focus, on the first selected prim that has it. False if there's
    // nothing to show; `why` then says why.
    bool setContext(const PrimvarViewContext& context, QString* why = nullptr);
    const std::vector<Column>& columns() const { return m_columns; }
    // "6 points", "8 face corners"; empty when there's nothing to show.
    const QString& domain() const { return m_domain; }
    // The prim shown, and what its rows are as PrimvarElements has it ("points"); empty for a
    // constant one, or one whose elements USD doesn't name.
    const PXR_NS::SdfPath& prim() const { return m_prim; }
    const std::string& elementDomain() const { return m_elementDomain; }
    // The rows' text, tab-separated, with a line of column names first: for Copy.
    QString text(const QModelIndexList& indexes) const;
    // What an edited cell becomes: the view's, which writes it and says whether it could.
    void setCellEditor(std::function<bool(int column, std::size_t row, const QString& text)> editor) { m_editor = std::move(editor); }
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QString cell(const Column& column, std::size_t row, bool* problem = nullptr) const;
    // The face that corner `row` is on, from m_faceStarts.
    std::size_t faceOf(std::size_t corner) const;
    // A cell's value as it's edited: every digit, no index.
    QString editText(const Column& column, std::size_t row) const;
    std::function<bool(int, std::size_t, const QString&)> m_editor;

    std::vector<Column> m_columns;
    std::size_t m_rows = 0;
    bool m_constant = false;  // one row, each value whole
    PXR_NS::SdfPath m_prim;
    std::string m_elementDomain;
    QString m_domain;
    std::vector<std::size_t> m_faceStarts;  // a mesh's first corner of each face, for faceVarying
    PXR_NS::VtIntArray m_faceVertexIndices;
};

class USDCC_USD_UI_API PrimvarValuesView : public PrimvarView {
    Q_OBJECT

public:
    explicit PrimvarValuesView(QWidget* parent = nullptr);
    void setContext(const PrimvarViewContext& context, bool focusOnly) override;

    QTableView* table() const { return m_table; }
    PrimvarValuesModel* model() const { return m_model; }

    // Editing (docs/PRIMVAR_VIEWER.md §20), each one undo step: a cell's text (numbers for a
    // number's, text for a string's), and a change to rows of a column's primvar. False, with the
    // reason on the report line, if it wasn't done.
    bool editCell(int column, std::size_t row, const QString& text);
    bool changeValues(const QString& primvar, std::vector<std::size_t> rows, const PrimvarValueChange& change);
    // The rows picked in the table (or the current one), each once.
    std::vector<std::size_t> pickedRows() const;
    // How the last edit went; empty before one.
    QString lastReport() const;
    // What Set, Add, Multiply, Clamp and Remap ask: a dialog unless set (tests set it). Nothing: cancelled.
    using AskChange = std::function<std::optional<PrimvarValueChange>(PrimvarValueChange::Kind kind, const QString& primvar,
                                                                       const QStringList& components, bool text)>;
    void setAskChange(AskChange ask) { m_askChange = std::move(ask); }
    // The Change submenu for `column`'s primvar, as the table's menu has it.
    void addChangeMenu(QMenu* menu, int column);
    // The selected cells as copy() puts them on the clipboard.
    QString selectedText() const;
    void copy() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Rows picked here are the stage's primvar elements (Stage::primvarElements()), and the row
    // under the cursor its hover; the stage's are picked here when they're of these rows.
    void rowsPicked();
    void showStageElements();
    PrimvarElements elementsOf(std::vector<std::size_t> rows) const;

    StageRefPtr m_stage;
    UndoManager* m_undo = nullptr;
    usdcc::core::Signal<>::Connection m_elementsConnection;
    bool m_syncing = false;
    AskChange m_askChange;
    QLabel* m_status;
    QLabel* m_report;
    QTableView* m_table;
    PrimvarValuesModel* m_model;
    std::string m_shown;  // the prim and primvar shown, to keep the scroll position for them
};

}  // namespace usdcc::usd
