#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QString>

#include <memory>
#include <vector>

class QAction;
class QLineEdit;

namespace usdcc::usd {

class PropertySheet;
class StageManager;
class UndoManager;
class VariantEditBanner;

// ViewPanel subclass showing and editing the properties of the selected prim
// (the first one, when several are selected) on this panel's stage — see
// docs/ATTRIBUTE_EDITOR.md (milestone M13, phase 3).
//
// The rows are a PropertySheet in Full mode (property_sheet.h). Around them: a
// toolbar with a search field, an options menu that filters to authored
// properties and shows hidden ones (both persist), and the banner shown while a
// variant is being edited.
class USDCC_USD_UI_API AttributesViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit AttributesViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                  const QString& title = QStringLiteral("Attributes"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // The Applied Schemas row's + menu (docs/APPLIED_SCHEMAS.md §4): the
    // registered schemas by category, greyed with the reason when unavailable.
    void populateSchemaMenu(QMenu* menu);

    PropertySheet* sheet() const { return m_sheet; }

protected:
    // Authored Only and Hidden Properties.
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private Q_SLOTS:
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);

private:
    void updateVariantDetail();

    std::shared_ptr<UndoManager> m_undoManager;
    QLineEdit* m_search;
    QAction* m_authoredOnly;
    QAction* m_showHidden;
    VariantEditBanner* m_variantBanner;
    PropertySheet* m_sheet;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
};

}  // namespace usdcc::usd
