#include "usdcc/usd/promote_schema_dialog.h"

#include "usdcc/usd/schema_explorer_view_panel.h"
#include "usdcc/usd/schema_libraries.h"
#include "usdcc/usd/schema_library_editor.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace usdcc::usd {

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }
enum Column { Name, Type, Fallback, ColumnCount };
constexpr auto kNewLibrary = "\n";  // the "New Library..." entry's data: no library is named so
}  // namespace

PromoteSchemaDialog::PromoteSchemaDialog(const PXR_NS::UsdPrim& prim, const QStringList& checked, QWidget* parent)
    : QDialog(parent), m_properties(promotableProperties(prim)) {
    setWindowTitle(tr("Make Schema from Attributes"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("An API schema with these properties of %1, their values as its fallbacks. Their names "
                                "stay as they are, so once it's generated and loaded the prims that have them can be "
                                "given it.")
                                 .arg(qs(prim.GetPath().GetString())),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("promoteProperties"));
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Property"), tr("Type"), tr("Fallback")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setStretchLastSection(true);
    for (const SchemaPropertyDef& property : m_properties) {
        auto* item = new QTreeWidgetItem(m_list, {qs(property.name),
                                                  property.relationship ? tr("relationship")
                                                                        : (property.uniform ? tr("uniform %1") : QStringLiteral("%1"))
                                                                              .arg(qs(property.typeName)),
                                                  qs(property.fallback)});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Name, checked.isEmpty() || checked.contains(qs(property.name)) ? Qt::Checked : Qt::Unchecked);
    }
    m_list->resizeColumnToContents(Name);
    connect(m_list, &QTreeWidget::itemChanged, this, [this]() { suggestName(); });
    layout->addWidget(m_list, 1);

    auto* form = new QFormLayout;
    m_library = new QComboBox(this);
    m_library->setObjectName(QStringLiteral("promoteLibrary"));
    for (const SchemaLibraryLocation& location : SchemaLibraries::instance().libraries()) {
        if (location.writable) m_library->addItem(qs(location.name), qs(location.name));
    }
    m_library->addItem(tr("New Library..."), QLatin1String(kNewLibrary));
    connect(m_library, &QComboBox::currentIndexChanged, this, [this]() { suggestName(); });
    form->addRow(tr("Library"), m_library);
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("promoteName"));
    connect(m_name, &QLineEdit::textEdited, this, [this]() { m_nameEdited = true; });
    form->addRow(tr("API name"), m_name);
    layout->addLayout(form);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(tr("Make Schema"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &PromoteSchemaDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(560, 420);
    suggestName();
}

std::vector<SchemaPropertyDef> PromoteSchemaDialog::chosen() const {
    std::vector<SchemaPropertyDef> result;
    for (int i = 0; i < m_list->topLevelItemCount() && i < int(m_properties.size()); ++i) {
        if (m_list->topLevelItem(i)->checkState(Name) == Qt::Checked) result.push_back(m_properties[std::size_t(i)]);
    }
    return result;
}

QString PromoteSchemaDialog::library() const {
    const QString data = m_library->currentData().toString();
    return data == QLatin1String(kNewLibrary) ? QString() : data;
}

QString PromoteSchemaDialog::apiName() const { return m_name->text().trimmed(); }

void PromoteSchemaDialog::setChosen(const QStringList& names) {
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_list->topLevelItem(i);
        item->setCheckState(Name, names.contains(item->text(Name)) ? Qt::Checked : Qt::Unchecked);
    }
}

void PromoteSchemaDialog::setLibrary(const QString& library) {
    const int index = m_library->findData(library);
    if (index >= 0) m_library->setCurrentIndex(index);
}

void PromoteSchemaDialog::setApiName(const QString& name) {
    m_name->setText(name);
    m_nameEdited = true;
}

void PromoteSchemaDialog::suggestName() {
    SchemaLibraryDef definition;
    if (const auto location = SchemaLibraries::instance().find(library().toStdString())) {
        readSchemaLibrary(location->source(), &definition, nullptr);
    }
    if (definition.name.empty()) definition.name = library().isEmpty() ? std::string("my") : library().toStdString();
    if (!m_nameEdited) m_name->setText(qs(suggestedApiName(definition, chosen())));
    m_note->setText(m_properties.empty() ? tr("The prim has no properties that no schema defines.") : QString());
}

bool PromoteSchemaDialog::apply(QString* error) {
    const std::vector<SchemaPropertyDef> properties = chosen();
    if (properties.empty()) {
        if (error) *error = tr("Tick the properties the schema should have");
        return false;
    }
    QString target = library();
    if (target.isEmpty()) {
        bool ok = false;
        const QString name =
            QInputDialog::getText(this, tr("New Schema Library"), tr("Name, as its schemas' namespace:"), QLineEdit::Normal, {}, &ok)
                .trimmed();
        if (!ok || name.isEmpty()) {
            if (error) error->clear();
            return false;
        }
        std::string why;
        if (!SchemaLibraries::instance().create(name.toStdString(), nullptr, &why)) {
            if (error) *error = qs(why);
            return false;
        }
        m_library->insertItem(0, name, name);
        m_library->setCurrentIndex(0);
        target = name;
    }
    std::string why;
    if (!addApiToLibrary(target.toStdString(), apiName().toStdString(), properties, &why)) {
        if (error) *error = qs(why);
        return false;
    }
    return true;
}

void PromoteSchemaDialog::accept() {
    QString error;
    if (!apply(&error)) {
        if (!error.isEmpty()) QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QDialog::accept();
}

bool promoteToSchemaInteractively(QWidget* parent, const PXR_NS::UsdPrim& prim, const QStringList& checked) {
    if (!prim) return false;
    PromoteSchemaDialog dialog(prim, checked, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    // On to the editor, to review it and Generate.
    if (const auto& show = schemaEditorHooks().showSchemasPanel) {
        if (SchemaExplorerViewPanel* panel = show()) panel->editLibrary(dialog.library(), dialog.apiName());
    }
    return true;
}

}  // namespace usdcc::usd
