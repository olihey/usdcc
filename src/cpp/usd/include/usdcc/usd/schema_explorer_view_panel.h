#pragma once

#include "usdcc/core/export.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/schema_info.h"
#include "usdcc/usd/stage.h"

#include <map>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QUrl;

namespace usdcc::usd {

class StageManager;
class UsdaPreviewDialog;

// The Schemas panel (docs/SCHEMAS.md §4): every schema USD has registered, by
// plugin, typed schemas nested by inheritance and API schemas after them; for
// the chosen one its kind, plugin, inheritance, built-in and auto-applied APIs,
// documentation and properties, each with the schema that declares it. Names
// link to their schemas. Show Source opens the plugin's schema.usda at the
// schema's class. Read-only, and the same whatever the stage.
class USDCC_USD_UI_API SchemaExplorerViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // What the tree lists.
    enum class Show { All, PrimTypes, AbstractTypes, APISchemas, Codeless };

    explicit SchemaExplorerViewPanel(StageManager* stageManager, StageRefPtr stage,
                                     const QString& title = QStringLiteral("Schemas"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

    // As of the last refresh, by name.
    const std::vector<SchemaInfo>& schemas() const { return m_schemas; }
    // The chosen schema, or null.
    const SchemaInfo* currentSchema() const;
    // Chooses `name` ("Mesh"; an instance names its schema), clearing a filter
    // that hides it. False if USD doesn't know it.
    bool selectSchema(const QString& name);
    void setFilterText(const QString& text);
    void setShow(Show show);

    QTreeWidget* tree() const { return m_tree; }
    QTreeWidget* propertyTree() const { return m_properties; }
    QTextBrowser* details() const { return m_details; }

    // Opens the schema's schema.usda (else its generatedSchema.usda) in the
    // Show USDA dialog, at its class; `generated`: its generatedSchema.usda,
    // what USD reads. Null if there's no such file.
    UsdaPreviewDialog* showSource(const QString& name, bool generated = false);
    // What a link in the details does: "schema:Mesh" chooses Mesh,
    // "source:Mesh" and "generated:Mesh" show its files.
    void openLink(const QUrl& url);

public Q_SLOTS:
    // Reads the registry again: a plugin may have registered schemas since.
    void refresh();

protected:
    void populateOptionsMenu(QMenu* menu) override;

private:
    void rebuildTree();
    void applyFilter();
    void showDetails();
    void showMenu(const QPoint& pos);
    QString detailsHtml(const SchemaInfo& info) const;
    QString schemaLink(const std::string& name) const;
    const SchemaInfo* find(const std::string& name) const;

    QLineEdit* m_filter;
    QComboBox* m_show;
    QLabel* m_summary;
    QTreeWidget* m_tree;
    QTextBrowser* m_details;
    QTreeWidget* m_properties;
    std::vector<SchemaInfo> m_schemas;
    std::map<std::string, std::size_t> m_index;   // name to m_schemas
    std::map<std::string, QTreeWidgetItem*> m_items;
};

}  // namespace usdcc::usd
