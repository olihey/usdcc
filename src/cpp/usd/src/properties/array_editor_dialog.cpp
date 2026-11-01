#include "usdcc/usd/properties/array_editor_dialog.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace usdcc::usd::properties {

using namespace PXR_NS;

// Rows are array elements; the only column is the element's value. Element
// text is produced on demand, so huge arrays cost nothing until scrolled to.
class ArrayModel : public QAbstractTableModel {
public:
    ArrayModel(VtValue array, SdfValueTypeName elementType, QObject* parent)
        : QAbstractTableModel(parent), m_array(std::move(array)), m_elementType(std::move(elementType)) {}

    const VtValue& array() const { return m_array; }
    const SdfValueTypeName& elementType() const { return m_elementType; }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(arraySize(m_array));
    }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 1; }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};
        return QString::fromStdString(formatValue(element(index.row()), 4));
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole) return {};
        return orientation == Qt::Horizontal ? QVariant(tr("Value")) : QVariant(section);
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        return QAbstractTableModel::flags(index) | Qt::ItemIsEditable;
    }

    VtValue element(int row) const { return arrayElement(m_array, static_cast<size_t>(row)); }
    void setElement(int row, const VtValue& value) {
        if (setArrayElement(m_array, static_cast<size_t>(row), value)) {
            Q_EMIT dataChanged(index(row, 0), index(row, 0));
        }
    }
    void insertAt(int row, const VtValue& value) {
        beginInsertRows({}, row, row);
        insertArrayElement(m_array, static_cast<size_t>(row), value);
        endInsertRows();
    }
    void removeAt(int row) {
        beginRemoveRows({}, row, row);
        removeArrayElement(m_array, static_cast<size_t>(row));
        endRemoveRows();
    }
    void reset(VtValue array) {
        beginResetModel();
        m_array = std::move(array);
        endResetModel();
    }
    void resizeTo(size_t size) {
        VtValue copy = m_array;
        resizeArray(copy, size);
        reset(std::move(copy));
    }

private:
    VtValue m_array;
    SdfValueTypeName m_elementType;
};

namespace {

// Edits one element with the element type's own PropertyEditor.
class ElementDelegate : public QStyledItemDelegate {
public:
    ElementDelegate(EditorHints hints, QObject* parent) : QStyledItemDelegate(parent), m_hints(std::move(hints)) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const auto* model = static_cast<const ArrayModel*>(index.model());
        PropertyEditor* editor = createPropertyEditor(model->elementType(), m_hints, parent);
        editor->setAutoFillBackground(true);
        // Commit as soon as an edit gesture ends, not only when the cell loses focus.
        connect(editor, &PropertyEditor::editFinished, this,
                [this, editor]() { const_cast<ElementDelegate*>(this)->commitData(editor); });
        return editor;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        static_cast<PropertyEditor*>(editor)->setValue(static_cast<const ArrayModel*>(index.model())->element(index.row()));
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        static_cast<ArrayModel*>(model)->setElement(index.row(), static_cast<PropertyEditor*>(editor)->value());
    }
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        editor->setGeometry(option.rect.adjusted(2, 1, -2, -1));
    }

private:
    EditorHints m_hints;
};

// Parses one pasted line into an element, or returns an empty value.
VtValue parseElement(const QString& line, const SdfValueTypeName& elementType) {
    const ValueTypeInfo info = ValueTypeInfo::classify(elementType);
    const QString trimmed = line.trimmed();
    if (info.isNumeric() || info.shape == ValueShape::Matrix) {
        std::vector<double> numbers;
        static const QRegularExpression separators(QStringLiteral("[\\s,;()\\[\\]]+"));
        for (const QString& part : trimmed.split(separators, Qt::SkipEmptyParts)) {
            bool ok = false;
            numbers.push_back(part.toDouble(&ok));
            if (!ok) return {};
        }
        return makeNumericValue(elementType, numbers);
    }
    switch (info.scalarKind) {
        case ScalarKind::Bool:
            return VtValue(trimmed == QLatin1String("1") || trimmed.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
        case ScalarKind::String:
            return VtValue(trimmed.toStdString());
        case ScalarKind::Token:
            return VtValue(TfToken(trimmed.toStdString()));
        case ScalarKind::Asset:
            return VtValue(SdfAssetPath(trimmed.toStdString()));
        default:
            return {};
    }
}

}  // namespace

ArrayEditorDialog::ArrayEditorDialog(const VtValue& array, const SdfValueTypeName& arrayType, EditorHints hints,
                                     const QString& title, QWidget* parent)
    : QDialog(parent), m_arrayType(arrayType) {
    setWindowTitle(tr("Edit %1").arg(title));
    resize(520, 480);

    m_model = new ArrayModel(array.IsEmpty() ? emptyArray(arrayType) : array, arrayType.GetScalarType(), this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new ElementDelegate(std::move(hints), m_view));
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->verticalHeader()->setDefaultSectionSize(24);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::SelectedClicked);

    auto* add = new QPushButton(tr("Add"), this);
    auto* insert = new QPushButton(tr("Insert"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    auto* resizeButton = new QPushButton(tr("Resize\xE2\x80\xA6"), this);
    auto* pasteButton = new QPushButton(tr("Paste Lines"), this);
    pasteButton->setToolTip(tr("Replace the array with the clipboard's lines, one element per line"));
    m_countLabel = new QLabel(this);

    auto* tools = new QHBoxLayout;
    tools->addWidget(add);
    tools->addWidget(insert);
    tools->addWidget(remove);
    tools->addWidget(resizeButton);
    tools->addWidget(pasteButton);
    tools->addStretch(1);
    tools->addWidget(m_countLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(tools);
    layout->addWidget(m_view, 1);
    layout->addWidget(buttons);

    connect(add, &QPushButton::clicked, this, &ArrayEditorDialog::addElement);
    connect(insert, &QPushButton::clicked, this, &ArrayEditorDialog::insertElement);
    connect(remove, &QPushButton::clicked, this, &ArrayEditorDialog::removeElements);
    connect(resizeButton, &QPushButton::clicked, this, &ArrayEditorDialog::resizeElements);
    connect(pasteButton, &QPushButton::clicked, this, &ArrayEditorDialog::paste);
    updateCount();
}

VtValue ArrayEditorDialog::array() const { return m_model->array(); }

void ArrayEditorDialog::updateCount() { m_countLabel->setText(tr("%n element(s)", nullptr, m_model->rowCount())); }

void ArrayEditorDialog::addElement() {
    const int rows = m_model->rowCount();
    // A copy of the last element is usually a better start than zero.
    VtValue element = rows > 0 ? m_model->element(rows - 1) : VtValue();
    if (element.IsEmpty()) {
        VtValue one = emptyArray(m_arrayType);
        resizeArray(one, 1);
        element = arrayElement(one, 0);
    }
    m_model->insertAt(rows, element);
    m_view->selectRow(rows);
    m_view->scrollToBottom();
    updateCount();
}

void ArrayEditorDialog::insertElement() {
    const QModelIndex current = m_view->currentIndex();
    const int row = current.isValid() ? current.row() : 0;
    VtValue one = emptyArray(m_arrayType);
    resizeArray(one, 1);
    m_model->insertAt(row, arrayElement(one, 0));
    m_view->selectRow(row);
    updateCount();
}

void ArrayEditorDialog::removeElements() {
    std::set<int> rows;
    for (const QModelIndex& index : m_view->selectionModel()->selectedRows()) rows.insert(index.row());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) m_model->removeAt(*it);
    updateCount();
}

void ArrayEditorDialog::resizeElements() {
    bool ok = false;
    const int size = QInputDialog::getInt(this, tr("Resize Array"), tr("Number of elements:"), m_model->rowCount(), 0,
                                          100000000, 1, &ok);
    if (ok) {
        m_model->resizeTo(static_cast<size_t>(size));
        updateCount();
    }
}

void ArrayEditorDialog::paste() {
    const QStringList lines = QApplication::clipboard()->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    VtValue result = emptyArray(m_arrayType);
    for (const QString& line : lines) {
        const VtValue element = parseElement(line, m_arrayType.GetScalarType());
        if (element.IsEmpty() || !insertArrayElement(result, arraySize(result), element)) {
            m_countLabel->setText(tr("Could not parse \"%1\"").arg(line.left(40)));
            return;
        }
    }
    m_model->reset(result);
    updateCount();
}

}  // namespace usdcc::usd::properties
