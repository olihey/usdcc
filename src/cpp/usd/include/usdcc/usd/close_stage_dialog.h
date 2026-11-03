#pragma once

#include "usdcc/usd/stage.h"

#include <QDialog>
#include <QStringList>

#include <vector>

class QComboBox;
class QRadioButton;
class QWidget;

namespace usdcc::usd {

class StageManager;

// File > Close Stage: closes `stage`. If panels are still showing it, first
// asks whether to close those panels or switch them all to another open stage
// (or to none). Returns false if the user cancelled, true once the stage is
// closed.
// It asks about unsaved layers first (confirmUnsavedLayers()).
bool closeStageInteractively(StageManager* stageManager, const StageRefPtr& stage, QWidget* dialogParent);

// Before `closing` stages close (docs/LAYER_EDITOR.md §5): if they use file
// layers with unsaved changes that no other open stage of `stageManager`
// still uses, lists them and asks Save All, Discard or Cancel. True to go
// on closing (nothing unsaved, everything saved, or Discard); false for
// Cancel, or when a save failed (which it says).
bool confirmUnsavedLayers(StageManager* stageManager, const std::vector<StageRefPtr>& closing, QWidget* dialogParent);

// The question closeStageInteractively() asks: "Close these panels" or
// "Switch them to: [stage]". The switch list holds every other open stage,
// then "No stage", which leaves the panels empty.
class CloseStageDialog : public QDialog {
    Q_OBJECT

public:
    CloseStageDialog(StageManager* stageManager, const StageRefPtr& stage, const QStringList& panelTitles,
                     QWidget* parent = nullptr);

    bool closePanels() const;
    // The stage to switch the panels to; null for "No stage".
    StageRefPtr replacement() const;

private:
    StageManager* m_stageManager;
    QRadioButton* m_closeButton;
    QRadioButton* m_switchButton;
    QComboBox* m_replacementCombo;
};

}  // namespace usdcc::usd
