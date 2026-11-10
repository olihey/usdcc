#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/property.h>

#include <QString>
#include <QWidget>

#include <memory>
#include <set>
#include <string>
#include <vector>

class QLabel;
class QMenu;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace usdcc::usd {

class StageManager;
class UndoManager;
struct PropertyCategory;
struct ShaderInputInfo;

namespace properties {
class AttributeBinding;
class CategorySection;
class PropertyEditor;
class PropertyRow;
class SchemaPillsEditor;
class TransformStackEditor;
}  // namespace properties

// The rows that show and edit one prim's properties (docs/ATTRIBUTE_EDITOR.md, and
// docs/MATERIAL_EDITOR.md §7.1). The Attribute Editor hosts one in Full mode; the
// Material Editor's Parameters pane hosts one in Shading mode.
//
// Full: a "Prim" header (name, which renames the prim, type, kind, specifier, active,
// instanceable, applied schemas), a "Composition" section, then one collapsible section
// per PropertyCategorizer category, each row a label plus the value type's own
// PropertyEditor. A Shader's inputs are in its definition's pages instead, with the
// ones not authored yet dimmed: editing one creates it.
//
// Shading: only inputs. A Shader's are in its definition's pages; a Material's or
// NodeGraph's are its interface, in their display groups.
//
// Attribute edits go through AttributeBinding (one undo step per gesture). The sheet
// follows the stage live (Stage::changed): value changes update the rows in place,
// and only a change to the set of properties shown rebuilds them, never in the middle
// of an edit gesture, which would delete the widget being dragged.
class USDCC_USD_UI_API PropertySheet : public QWidget {
    Q_OBJECT

public:
    enum class Mode { Full, Shading };

    PropertySheet(StageManager* stageManager, std::shared_ptr<UndoManager> undoManager, Mode mode,
                  QWidget* parent = nullptr);
    ~PropertySheet() override;

    Mode mode() const { return m_mode; }

    void setStage(const StageRefPtr& stage);
    const StageRefPtr& stage() const { return m_stage; }
    // Rebuilds when it's another prim, or `force`.
    void setPrimPath(const PXR_NS::SdfPath& path, bool force = false);
    const PXR_NS::SdfPath& primPath() const { return m_primPath; }
    PXR_NS::UsdPrim currentPrim() const;

    // What the rows are filtered by: the label or name contains it, case-insensitively.
    void setSearchText(const QString& text);
    void setAuthoredOnly(bool authoredOnly);
    bool authoredOnly() const { return m_authoredOnly; }
    void setShowHidden(bool showHidden);
    bool showHidden() const { return m_showHidden; }
    // What's said when there's no prim to show; a default when empty.
    void setEmptyText(const QString& text);

    // The Applied Schemas row's + menu (docs/APPLIED_SCHEMAS.md §4): the
    // registered schemas by category, greyed with the reason when unavailable.
    void populateSchemaMenu(QMenu* menu);

    void rebuild();
    QScrollArea* scrollArea() const { return m_scroll; }

Q_SIGNALS:
    // The rows were made again, for currentPrim().
    void rebuilt();

protected:
    // Tab and Shift+Tab in the rows (docs/ATTRIBUTE_EDITOR.md §4.4): commit the field
    // and start typing in the next one to the right, else the first one on a line
    // below, wrapping round at the end. Fields are number and text fields that can be
    // typed in now (not read-only, not in a collapsed section).
    bool focusNextPrevChild(bool next) override;

private:
    struct Row {
        properties::PropertyRow* row = nullptr;
        properties::AttributeBinding* binding = nullptr;  // null for relationships
        properties::PropertyEditor* editor = nullptr;
        properties::CategorySection* section = nullptr;
        QWidget* frame = nullptr;  // the frame around it, for primvars of one namespace
        PXR_NS::UsdProperty property;
        // A relationship's row; it can exist before the relationship does.
        PXR_NS::TfToken relationship;
        // A shader input with no attribute yet: its row, which editing creates.
        PXR_NS::TfToken pendingInput;
        QString searchText;  // label and name, lower case
        bool managedByStack = false;  // a transform-stack row (TransformStackEditor owns its actions)
        // UsdShade (shade_connections.h): a receiver's connect/disconnect button, and
        // whether this is a shader output, whose editor lists the inputs it feeds.
        QToolButton* connectButton = nullptr;
        bool shaderOutput = false;
    };
    struct PrimHeader {
        properties::PropertyEditor* name = nullptr;
        properties::PropertyEditor* type = nullptr;
        properties::PropertyEditor* kind = nullptr;
        properties::PropertyEditor* specifier = nullptr;
        properties::PropertyEditor* active = nullptr;
        properties::PropertyEditor* instanceable = nullptr;
        properties::SchemaPillsEditor* schemas = nullptr;
    };

    void refreshValues();
    void refreshPrimHeader(const PXR_NS::UsdPrim& prim);
    // From the Name row: one undo step (prim_actions::rename()); the selection
    // follows the new path. A name that isn't valid or is taken is refused.
    void renameCurrentPrim(const std::string& name);
    void updateRowState(Row& row);
    void applyFilter();
    void buildPrimSection(const PXR_NS::UsdPrim& prim, QVBoxLayout* layout);
    // Shading mode's first line: the prim's name and what it is.
    void buildShadingTitle(const PXR_NS::UsdPrim& prim, QVBoxLayout* layout);
    // A Shader's inputs in its definition's pages, authored or not.
    void buildShaderPages(const PXR_NS::UsdPrim& prim, QVBoxLayout* layout);
    Row buildShaderInputRow(const PXR_NS::UsdPrim& prim, const ShaderInputInfo& input);
    void buildTransformStack(const PXR_NS::UsdPrim& prim, properties::CategorySection* section,
                             std::set<PXR_NS::TfToken>& inStack);
    // `primvarGroup`: the namespace of the frame it's going in, which its label leaves out.
    Row buildRow(const PXR_NS::UsdProperty& property, const PropertyCategory& category,
                 const std::string& primvarGroup = {});
    // An xformOp:* attribute xformOpOrder doesn't list: dimmed, a note naming
    // the layer that keeps it alive, and a context-menu "Add to Stack".
    void setUpUnusedOp(const Row& row);
    properties::CategorySection* makeSection(const QString& key, const QString& title, bool collapsedByDefault = false);
    void buildCompositionSection(const PXR_NS::UsdPrim& prim, QVBoxLayout* layout);
    // From the section's +: a reference or payload (a file dialog) or an
    // inherit or specialize (a prim path).
    void addArcInteractively(int type);
    // A set's row in the Composition section: its pencil and its actions.
    void addVariantSetRowActions(properties::PropertyRow* row, const std::string& set);
    // The pills' × and + (docs/APPLIED_SCHEMAS.md §4, §5): removing asks about
    // the authored properties it would leave behind; applying, a multiple-apply
    // schema asks for an instance name, then whether to add its attributes.
    void removeSchemaInteractively(const QString& schema);
    void applySchemaInteractively(const QString& schema, bool multipleApply);
    // A relationship's row (docs/RELATIONSHIPS.md): its targets as pills that
    // select them, and a dialog of the stage's prims to add one.
    void buildRelationshipEditor(Row& row, const PXR_NS::TfToken& name, const QString& label);
    void addRelationshipTargetInteractively(const PXR_NS::TfToken& name);
    // UsdShade connections (docs/ATTRIBUTE_EDITOR.md §4.3): a receiver's
    // connect/disconnect button, and a shader output's pills of the inputs it feeds.
    void addConnectionButton(Row& row, const PXR_NS::SdfPath& attribute);
    void buildShaderOutputEditor(Row& row, const PXR_NS::SdfPath& output, const QString& label);
    // The dialog, from a receiver (pick its source) or a shader output (pick an input).
    void connectInteractively(const PXR_NS::SdfPath& attribute, bool fromOutput);
    void disconnectReceiver(const PXR_NS::SdfPath& receiver);
    // Selects the prim of `path` (a prim or property path), after the current event.
    void selectPrimLater(const PXR_NS::SdfPath& path);
    void onStageObjectsChanged(const StageChanges& changes);
    void processPendingChanges();
    void queueProcessPendingChanges();
    bool anyEditing() const;
    // The fields Tab goes through, in reading order: by line, top to bottom, then
    // left to right. `including`, if given, is placed among them wherever it is, so
    // that Tab can move on from a check box or combo box too.
    std::vector<QWidget*> tabStops(QWidget* including = nullptr) const;
    std::string layoutSignature(const PXR_NS::UsdPrim& prim) const;
    QString collapsedKeyPrefix() const;

    StageManager* m_stageManager;
    std::shared_ptr<UndoManager> m_undoManager;
    Mode m_mode;
    StageRefPtr m_stage;
    PXR_NS::SdfPath m_primPath;
    // With shader outputs shown: their material, edits anywhere under which change
    // their pills (empty otherwise).
    PXR_NS::SdfPath m_connectionScope;

    QString m_searchText;
    QString m_emptyText;
    bool m_authoredOnly = false;
    bool m_showHidden = false;
    QScrollArea* m_scroll;
    QWidget* m_content = nullptr;

    std::vector<Row> m_rows;
    std::vector<properties::CategorySection*> m_sections;
    properties::CategorySection* m_primSection = nullptr;
    properties::CategorySection* m_compositionSection = nullptr;
    properties::TransformStackEditor* m_xformEditor = nullptr;
    PrimHeader m_header;
    std::string m_signature;  // what the current rows were built from

    bool m_rebuildPending = false;
    bool m_refreshPending = false;
    bool m_processQueued = false;

    // The shown stage's change notices (re-made whenever the stage changes).
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_editTargetConnection;
};

}  // namespace usdcc::usd
