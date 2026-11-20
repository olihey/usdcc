#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/schema_info.h"
#include "usdcc/usd/stage.h"

#include <map>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QStackedWidget;
class QTextBrowser;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QUrl;

namespace usdcc::usd {

class SchemaLibraryEditor;
class StageManager;
class UndoManager;
class UsdaPreviewDialog;

// The Schemas panel (docs/SCHEMAS.md §4): every schema USD has registered, by
// plugin, typed schemas nested by inheritance and API schemas after them; for
// the chosen one its kind, plugin, inheritance, built-in and auto-applied APIs,
// documentation and properties, each with the schema that declares it. Names
// link to their schemas. Show Source opens the plugin's schema.usda at the
// schema's class.
//
// With a stage, This stage at the bottom lists what the stage uses, with
// counts, and the types and API schemas it names that USD doesn't know. A
// schema's details say how many prims use it, and offer Select, Create (a
// prim type, where the Create menu would put it) and Apply to Selection (an
// API schema, asking what the Attribute Editor's + asks). They follow the
// stage's edits and the selection.
//
// Schema Libraries lists the codeless libraries usdcc can edit, each with its
// state (SchemaLibraries); Edit opens the Schema Editor in place of the
// details (docs/SCHEMAS.md §5), and New Schema Library makes one.
class USDCC_USD_UI_API SchemaExplorerViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // What the tree lists.
    enum class Show { All, PrimTypes, AbstractTypes, APISchemas, Codeless, OnStage };

    explicit SchemaExplorerViewPanel(StageManager* stageManager, StageRefPtr stage,
                                     std::shared_ptr<UndoManager> undoManager,
                                     const QString& title = QStringLiteral("Schemas"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    QJsonObject saveSessionState() const override;
    bool canClose() override;
    void restoreSessionState(const QJsonObject& state) override;

    // As of the last refresh, by name.
    const std::vector<SchemaInfo>& schemas() const { return m_schemas; }
    // The chosen schema, or null (also for an unregistered one).
    const SchemaInfo* currentSchema() const;
    // The chosen unregistered type or schema, from This stage; else empty.
    QString currentUnregistered() const;
    // What the stage uses, as of the last refresh.
    const StageSchemaUse& stageUse() const { return m_use; }
    // Chooses `name` among This stage's unregistered ones; false if it isn't one.
    bool selectUnregistered(const QString& name);

    // Selects the prims using `schema` (registered or not); false if none do.
    bool selectPrimsUsing(const QString& schema);
    // Creates a prim of `type` where the Create menu would (under the one
    // selected prim, else at the root), one undo step, and selects it. Empty if
    // refused; `why` says why.
    PXR_NS::SdfPath createPrim(const QString& type, QString* why = nullptr);
    // Where createPrim() would put a prim now: "under /World", or why it can't.
    QString createTargetText(bool* possible = nullptr) const;
    // Applies API schema `schema` to the selected prims, asking as the Attribute
    // Editor does (askAndApplySchema()). Whether anything was applied.
    bool applyToSelection(const QString& schema);

    // The stage's prims that author one of API schema `schema`'s properties
    // without having it applied (a schema made from them, docs/SCHEMAS.md §6).
    std::vector<PXR_NS::SdfPath> primsWithAttributesOf(const QString& schema) const;
    // Applies it to all of those, one undo step. Whether anything was applied.
    bool applyToPrimsWithItsAttributes(const QString& schema);

    // The Schema Editor (docs/SCHEMAS.md §5).
    SchemaLibraryEditor* editor() const { return m_editor; }
    bool isEditing() const;
    // Opens library `library` in the editor, at `schema` if given; false if it
    // isn't one usdcc can edit, or the user kept the edits of another open.
    Q_INVOKABLE bool editLibrary(const QString& library, const QString& schema = {});
    // Back to the details; false if the user cancelled saving.
    bool closeEditor();
    // Makes a library in the user's plugins folder, and edits it.
    bool newLibrary(const QString& name, QString* error = nullptr);
    // The chosen row of Schema Libraries, or empty.
    QString currentLibrary() const;
    bool selectLibrary(const QString& name);
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
    // "source:Mesh" and "generated:Mesh" show its files, "select:Mesh" selects
    // the prims using it, "create:Mesh" creates one, "apply:MotionAPI" applies it
    // to the selection, "prim:/World/A" selects that prim.
    void openLink(const QUrl& url);

public Q_SLOTS:
    // Reads the registry again: a plugin may have registered schemas since.
    void refresh();
    // Counts what the stage uses again; its edits do this by themselves.
    void refreshStageUse();
    // Asks for a name, then newLibrary().
    void newLibraryInteractively();

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildTree();
    void rebuildStageGroup();
    void rebuildLibraryGroup();
    // The tree beside the details when there's room, above them when narrow.
    void updateOrientation();
    void applyFilter();
    void showDetails();
    void updateDetailsText();
    void showMenu(const QPoint& pos);
    std::vector<PXR_NS::SdfPath> selection() const;
    QString detailsHtml(const SchemaInfo& info) const;
    QString unregisteredHtml(const std::string& name) const;
    QString stageUseHtml(const std::string& name) const;
    QString libraryHtml(const std::string& name) const;
    QString schemaLink(const std::string& name) const;
    const SchemaInfo* find(const std::string& name) const;

    QLineEdit* m_filter;
    QComboBox* m_show;
    QLabel* m_summary;
    QTreeWidget* m_tree;
    QTextBrowser* m_details;
    QTreeWidget* m_properties;
    QTimer* m_stageTimer;
    std::shared_ptr<UndoManager> m_undoManager;
    bool m_stageUseStale = false;  // edited while hidden: counted when shown
    StageSchemaUse m_use;
    QTreeWidgetItem* m_stageGroup = nullptr;
    QTreeWidgetItem* m_libraryGroup = nullptr;
    QStackedWidget* m_stack;
    QSplitter* m_split;
    SchemaLibraryEditor* m_editor;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    std::vector<SchemaInfo> m_schemas;
    std::map<std::string, std::size_t> m_index;   // name to m_schemas
    std::map<std::string, QTreeWidgetItem*> m_items;
};

}  // namespace usdcc::usd
