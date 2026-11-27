#pragma once

// Send to… in the app (docs/PEERS.md §10.3): File > Send To, and what this usdcc does with
// each kind it receives.

class QMenu;

namespace usdcc::scripting {
class ScriptEditorSidePanel;
class SessionController;
}  // namespace usdcc::scripting

namespace usdcc::ui {
class MainWindow;
class PeersController;
}  // namespace usdcc::ui

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::app {

struct SendToContext {
    usdcc::ui::PeersController* peers = nullptr;
    usdcc::ui::MainWindow* window = nullptr;
    usdcc::usd::StageManager* stages = nullptr;
    usdcc::usd::UndoManager* undo = nullptr;
    usdcc::scripting::SessionController* sessions = nullptr;
    usdcc::scripting::ScriptEditorSidePanel* scriptEditor = nullptr;
};

// Registers the receivers, and fills `menu` (File > Send To) each time it opens:
// - The current stage: its root layer's file, or with everything it uses (packed, as a render
//   job is), or its URL for a stage on a server. The receiver opens it.
// - The selected prims, as USDA: the receiver pastes them into its current stage.
// - This session: the receiver imports and loads it.
// - The Script Editor's tab: the receiver gets it as a new tab, never run.
// - The viewport's camera: the receiver's viewport looks from there.
void setUpSendTo(const SendToContext& context, QMenu* menu);

}  // namespace usdcc::app
