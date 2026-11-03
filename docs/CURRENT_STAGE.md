# The Current Stage — Plan

Status: **done** (both phases, §7), with the recommended choices in §6.

With several stages open, app-wide commands (Edit > Undo, a future Show All Prims, Save, a script
run) need to know which stage they act on. Make that the stage of the view panel the user last
worked in, as a multi-document app does with its documents, and show it in the window title.

## 1. What there was before

`StageManager::currentStage()` exists, with `setCurrentStage()` and a `currentStageChanged`
signal, but it is simply **the first stage opened**. It changes only when that stage is closed
(to the first one left) or when a script calls `usdcc.usd.set_current_stage()`. Focusing a panel
that shows another stage doesn't change it, and nothing shows which stage it is.

What reads it:

| Where | What for |
|---|---|
| Edit > Undo / Redo (`main.cpp`) | which stage's undo stack. **A bug today**: with two stages open, Undo while working in stage B undoes stage A's last edit |
| `ViewportToolBars` | the Target box (which xform op the tools drive) follows the current stage's selection |
| `usdcc.usd.current_stage()` / `set_current_stage()` | scripts' default stage |
| `usdcc.app.open_view_panel()` | the stage a new panel shows when none is given |

What doesn't, and shouldn't: each view panel (viewport, Outliner, Attributes) and its tools,
the Outliner's context menu, the Stages menu. They act on their own panel's stage.

## 2. The rule

**The current stage is the stage of the view panel that last had focus.**

- A `ViewPanel` gains focus (a click in it, Tab into it, its tab clicked): its stage becomes
  current. ADS already tracks this: `CDockManager::focusedDockWidgetChanged(old, now)`, with
  `FocusHighlighting` on (`MainWindow`).
- The focused panel switches its Stage dropdown: the new stage becomes current.
- Focus moves to anything else (the Script Editor, the Log, a dialog, a toolbar): the current stage
  stays, so a script run from the Script Editor acts on the stage you were just looking at.
- A panel with no stage gains focus: the current stage stays.
- The current stage is closed: the stage of the focused panel if it has one, else the first open
  stage, else none.
- A stage is opened: the viewport it is shown in (an empty one, or a new one, brought to the
  front) is focused and the stage becomes current (`main.cpp`, the `stageOpened` handler).
  Bringing a tab to the front alone doesn't move ADS's focus.
- `usdcc.usd.set_current_stage()` still sets it; the next panel focus overrides it.

## 3. Changes

| Where | Change |
|---|---|
| `ViewPanel` | A Qt signal `stageChanged(StageRefPtr)`, emitted by `setStage()` |
| New `CurrentStageTracker` (`usdcc_ui`, made in `main.cpp`) | Connects `focusedDockWidgetChanged` and the focused panel's `stageChanged` to `StageManager::setCurrentStage()`, with the rules in §2. A plain object, so it can be tested with a dock manager and panels |
| `StageManager::closeStage()` | Takes a hint for the replacement (the tracker passes the focused panel's stage), else the first open stage |
| `HydraViewportWindow` | A click in the viewport (a native `QWindow` inside `createWindowContainer()`, which may not move Qt widget focus) calls `CDockManager::setDockWidgetFocused()` on its panel, so clicking a viewport makes its stage current. Verify first; only if needed |
| Window title (`main.cpp`) | `usdcc — <stage name>` (the root layer's display name, or the short name from `shortStageNames()` when two stages share one), `usdcc` with none; follows `currentStageChanged` |
| Edit > Undo / Redo | Through a `QUndoGroup` whose active stack follows the current stage: the actions then say what they undo ("Undo Rename Chair") and are disabled when there is nothing to undo. Ctrl+Z / Ctrl+Y as now |
| `docs/SCRIPTING.md` | `current_stage()` explained as "the stage of the last focused view panel" |

`ViewportToolBars` needs no change: it already follows `currentStageChanged`.

## 4. Global tools from here on

A command that isn't tied to a panel acts on `StageManager::currentStage()`, and says so in its
undo text ("Show All Prims in kitchen.usda" when more than one stage is open). One that should
touch every open stage loops over `StageManager::stages()` and says so in its name ("Save All").

## 5. Tests (usdcc_ui_tests)

- Two stages, a panel on each: focusing either makes its stage current; focusing a side panel or
  a panel with no stage keeps it; switching the focused panel's dropdown changes it; closing the
  current stage falls back to the focused panel's.
- Undo with two stages: Ctrl+Z after focusing stage B's panel undoes B's edit, not A's; the
  action's text names the edit.
- The window title follows.

## 6. Decisions (as taken: the recommended ones)

1. **Focus, or "last clicked"?** Recommended: **focus** (ADS's own tracking), which also covers
   Tab and the tab bar. Only the viewport needs help, if its native window keeps focus from Qt.
2. **Show the current stage how?** Recommended: **the window title** ("usdcc — kitchen.usda").
   An extra marker on every panel showing the current stage (a dot on its tab) could come later;
   ADS already highlights the focused tab.
3. **Undo via `QUndoGroup`?** Recommended: **yes**: the menu then names the step and greys out
   when there is none, at almost no cost, since `UndoManager` already has one stack per stage.

## 7. Phases

| Phase | Work |
|---|---|
| **1. Follow focus** *(done)* | `ViewPanel::stageChanged`; `CurrentStageTracker` (usdcc_ui), made in `main.cpp`, which also handles the close fallback from `StageManager::stageClosed` (no `StageManager` change was needed); a viewport press (`HydraViewportWindow::pressed`) calls `setDockWidgetFocused()` on its panel, since ADS ignores the native window's focus |
| **2. Show and use it** *(done)* | `CurrentStageUndoGroup` and `windowTitle()` (`current_stage_ui.h`, usdcc_usd_ui), used by `main.cpp`; the scripting docs |

Tests: `tests/current_stage_test.cpp`. ADS reports focus only on a visible panel, which a focused
one always is in the app.
