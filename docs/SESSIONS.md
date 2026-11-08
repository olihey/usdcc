# Sessions (plan)

A session is a named snapshot of the app's working state: the open stages, the plugins, the panels and
where they are, and the Script Editor's tabs. Saving one writes a single JSON file; loading it puts the
app back as it was. A small dialog lists the sessions with a screenshot of each, and loads, saves,
renames, deletes, imports and exports them. A session is shared by sending its JSON file to someone.

Status: **phases 0 to 6 done** (§8.1 says what was built and where it differs from the plan). Planned
2026-11-08 against master `dbbf00a`.

## 1. What the app keeps today

Some of this already survives a restart, spread over `usdcc.ini` (`appSettings()`):

| State | Where | Key |
|---|---|---|
| Window geometry | `MainWindow::saveLayout()` | `MainWindow/geometry` |
| Toolbars and side panels (docked, floating, hidden) | same | `MainWindow/windowState` |
| Collapsed side panels | same | `MainWindow/collapsedSidePanels` |
| View panels' dock layout (ADS XML, compressed) | same | `MainWindow/dockLayout` |
| View panels opened from the Stages menu or duplicated: type, name, stage file | `ViewPanelRegistry::savePanels()` | `ViewPanels/opened` |
| A panel's pinned options | `ViewPanel::setOptionsPinned()` | `ViewPanel/optionsPinned/<objectName>` |
| Which plugins are on | `PluginManager::setEnabled()` | plugin keys |
| Script Editor tabs | `ScriptEditorSidePanel` | `ScriptEditor/tabs` |

What is **not** kept anywhere: the open stages themselves (a restart opens none, so restored panels come
back empty unless the stage was given on the command line), the current stage, cameras, renderers,
selections, edit targets, muted layers, loaded payloads, Outliner expansion.

So the layout half of a session exists; the stage half doesn't. A session reuses the layout code and adds
the rest.

## 2. What a session holds

### 2.1 Stages

Per open stage, in the order they were opened:

- **The root layer's file path** (absolute). This is what reopens it.
- **Whether it is the current stage.**
- **Its selection** (`StageManager::selectedPaths()`).
- **Its edit target**: the layer's identifier, or the session layer. Also the variant edit context, if one
  is entered (prim, set, variant).
- **Muted layers** (`UsdStage::GetMutedLayers()`). Muting isn't written to any file, so only a session
  can bring it back.
- **Load rules**: which payloads are loaded (`UsdStageLoadRules`, a list of path and rule). Without this a
  stage whose payloads were unloaded comes back with everything loaded, which can be slow.
- **The session layer's content**, if it isn't empty. Edits go there when it is the edit target, and they
  are lost on close otherwise. Stored as USDA text in the JSON.

**Not** stored: the layers' unsaved edits and the undo history. Saving warns about unsaved edits (§4.4).

**In-memory stages** (File > Create Stage > In Memory) have no file to reopen, so the session keeps their
root layer as USDA text under `inMemory`, like the session layer, and makes them again from it.

### 2.2 Plugins

- **The enabled plugins**: id and version each.
- Plugins can't be switched on or off while the app runs (`PluginManager`, docs/PLUGINS.md §3.4), so
  loading a session with a different set needs a restart. See §4.3.

### 2.3 Layout

The same four things `MainWindow::saveLayout()` writes today, plus the view panel list from
`ViewPanelRegistry::savePanels()`:

- Window geometry, and whether it is maximised.
- Toolbars and side panels: `QMainWindow::saveState()`, as base64.
- Collapsed side panels.
- Each docked side panel's size as a share of the window, and the window's size when saved, for a
  different screen (§5.2).
- The ADS dock layout, **uncompressed**: it is XML, so the JSON stays readable and diffable.
- Each view panel: type, objectName, which stage (by index into §2.1's list), whether its options are
  pinned.

The objectName is what ties a panel to its place in the dock layout, so it is kept as is.

### 2.4 Per-panel view state (new)

A new pair of virtuals on `ViewPanel`:

```cpp
virtual QJsonObject saveSessionState() const { return {}; }
virtual void restoreSessionState(const QJsonObject& state) {}
```

What each panel would store:

| Panel | State |
|---|---|
| Viewport | Orbit camera (`HydraViewportWindow::cameraState()`) and renderer. **Done.** (No scene camera to look through yet.) |
| Outliner | Expanded paths, search text, type filter, scroll position. **Done.** |
| USDA | What it shows (a layer, flattened, or a file) and the scroll position. **Done.** |
| Python panels | Whatever `save_state()` returns (anything that converts to JSON); `restore_state(state)` gets it back. **Done.** |
| Attributes, Material Editor | Nothing of their own: they show the selection, which the stage brings back. |
| Layer Editor, Dependencies | Nothing yet: they come back as they start. |

A panel that stores nothing comes back in its default state, so this can be done one panel at a time.

### 2.5 Script Editor tabs

All of them, as `ScriptEditorSidePanel::saveSession()` already writes them to the ini on quit:

- **Untitled (scratch) tabs**: name and text, always.
- **File tabs**: the path, and the text too while it has unsaved changes.
- **Plugin tabs**: the plugin's id and folder, its split, and its file tabs as above.
- Which tab is current.

`saveSession()`/`restoreSession()` are changed to write to and read from a `QJsonObject`; the ini keeps
using them through that. Loading a session replaces the tabs: a tab with unsaved changes is asked about
first, as quitting does (`canClose()`).

A file tab whose file doesn't exist (a session from someone else, §5.1) comes back with the stored text
if there is any, otherwise it is skipped with a warning.

### 2.6 What a session does **not** hold

- **Unsaved edits to layers.** Saving a session warns about them (§4.4).
- **Preferences** (Settings dialog, UI scale, theme, ...). These are the user's, not the session's.
- **Open Recent** and the Script Editor's recent files. They are the user's history across sessions.
- **Undo history.** USD can't serialise it.
- **The log.**

### 2.7 Plugins' own data

A plugin may want to keep something per session (a render setting, a chosen asset library). A Python hook:

```python
usdcc.app.on_session_save(lambda: {"preset": "draft"})  # stored under the plugin's id
usdcc.app.on_session_load(lambda data: ...)              # gets it back, or {} if there's none
usdcc.app.on_session_save(callback, key="studio.tools")  # outside a plugin: a key of its own
```

## 3. The file

### 3.1 Where

`<AppLocalData>/sessions/` (next to `usdcc.ini` and `plugins/`):

```
sessions/
  Kitchen lookdev.json
  Shot 040.json
```

- **One file per session, and nothing beside it.** Sharing a session means sending that file (§5.1), so
  everything it needs is inside: the preview, the session layers' text, the scratch scripts.
- **The file name is the session name.** Characters Windows forbids (`<>:"/\|?*`), trailing dots and
  spaces, and reserved names (`CON`, `NUL`, ...) are replaced with `_`. The real name is also stored in the
  JSON, so the dialog shows it as typed.
- Two names that differ only in case are the same file on Windows; the dialog treats them as the same
  session and asks before overwriting.
- **The preview is base64 PNG in the JSON** (§6.2), under `preview`, last in the file so the readable
  part comes first. That makes a file around 1 to 2 MB; the dialog reads the previews lazily (§6.1), so a
  long list stays quick.

### 3.2 Format

```json
{
  "format": "usdcc-session",
  "version": 1,
  "name": "Kitchen lookdev",
  "saved": "2026-11-08T14:32:10Z",
  "app": "0.9.0",
  "stages": [
    {
      "file": "C:/proj/kitchen/kitchen.usda",
      "current": true,
      "selection": ["/World/Counter/Top"],
      "editTarget": "C:/proj/kitchen/lookdev.usda",
      "variantEdit": null,
      "mutedLayers": ["C:/proj/kitchen/heavy_props.usda"],
      "loadRules": [["/", "AllRule"], ["/World/Props", "NoneRule"]],
      "sessionLayer": "#usda 1.0\nover \"World\" { ... }"
    }
  ],
  "plugins": [
    {"id": "usdcc.vray", "version": "0.3.0"}
  ],
  "layout": {
    "geometry": "AdnQywADAAAAAAA...",
    "maximized": true,
    "windowState": "AAAA/wAAAAD9AAAA...",
    "collapsedSidePanels": {"Log": 220},
    "windowSize": [2560, 1400],
    "sidePanelShares": {"ScriptEditor": 0.25, "Log": 0.18},
    "dockLayout": "<?xml version=\"1.0\"?><QtAdvancedDockingSystem ...>"
  },
  "panels": [
    {
      "type": "viewport", "name": "Viewport", "stage": 0, "optionsPinned": false,
      "state": {"camera": {"center": [0, 1, 0], "distance": 8.5, "yaw": 30, "pitch": -15}, "renderer": "HdStormRendererPlugin"}
    },
    {
      "type": "outliner", "name": "Outliner", "stage": 0, "optionsPinned": true,
      "state": {"expanded": ["/World", "/World/Counter"], "search": "", "types": []}
    }
  ],
  "scriptEditor": {
    "current": 1,
    "tabs": [
      {"name": "Untitled 1", "text": "import usdcc\nprint(usdcc.app.stages())\n"},
      {"file": "C:/proj/kitchen/scripts/fix_bindings.py"},
      {"plugin": "usdcc.vray", "name": "V-Ray", "folder": "C:/Users/me/AppData/Local/usdcc/plugins/vray",
       "split": "AAAA/wAAAAEAAAAC...", "current": 0, "files": [{"file": ".../vray/__init__.py"}]}
    ]
  },
  "pluginData": {
    "usdcc.vray": {"preset": "draft"}
  },
  "preview": "iVBORw0KGgoAAAANSUhEUgAABQAAAAMgCAIAAA..."
}
```

- `version` is raised when a change would make an old app misread a new file. An app that finds a newer
  version refuses to load it and says so; an older one is read as far as it goes.
- Unknown keys are ignored, so a plugin's or a later version's additions don't break loading.

## 4. Saving and loading

### 4.1 New classes

- **`SessionStore`** (usdcc_ui, Qt-only): the folder. `list()`, `read(name)`, `write(session, preview)`,
  `rename(from, to)`, `remove(name)`, `fileNameFor(name)`. Knows nothing of stages; tested on its own.
- **`Session`**: a plain struct for §3.2, with `toJson()` and `fromJson()`.
- **`SessionCapture`** (app layer, sees `StageManager`, `MainWindow`, `ViewPanelRegistry`,
  `PluginManager`): builds a `Session` from the running app.
- **`SessionLoader`** (same layer): applies one, step by step (§4.2), with a progress dialog.
- **`SessionsDialog`**: §6.

`MainWindow::saveLayout()`/`restoreLayout()` are split so the layout can be written to or read from a
`Session` as well as the ini, without two copies of the code.

### 4.2 Loading, in order

The order matters because each step needs the one before it:

1. **Plugins differ?** Then §4.3, and stop here.
2. **Unsaved edits?** Ask, as File > Close Stage does today (`confirmUnsavedLayers()`): Save, Discard,
   Cancel.
3. **Close everything**: every stage, and every view panel the registry made. The fixed Viewport, Outliner
   and Attributes stay, with no stage.
4. **Open the stages** with `StageOpener`, off the UI thread, one after another: "Opening 2 of 3:
   kitchen.usda". A file that's missing or fails to open is skipped and named in a warning at the end; its
   panels come back without a stage.
   - **Gotcha:** main.cpp gives every newly opened stage a viewport (`viewportOnStageOpened`). That must be
     switched off while a session loads, or each stage gets an extra viewport.
   - A stage with load rules or muted layers opens with `UsdStage::LoadNone`, then gets its muted layers
     (`MuteAndUnmuteLayers()`) and its rules (`SetLoadRules()`). So an unloaded payload is never loaded,
     even briefly. This needs a `StageManager::openStage()` option for the initial load set.
5. **Per stage**: session layer content, edit target, variant edit, selection.
6. **Make the panels** (`restorePanels()`, given the session's list instead of the ini's).
7. **Restore the layout**: window state, dock layout, collapsed side panels. The window's size and
   position only at start; on a load into the running app, only whether it is maximised (§9). Then
   floating panels are moved onto a screen and side panels scaled to the window (§5.2).
8. **Per panel**: pinned options, then `restoreSessionState()`. The viewport's camera waits until its GL
   context exists.
9. **Current stage** last, so the window title and the status bar follow it.
10. **Plugins' data**: `on_session_load` callbacks.

A step that fails logs a warning and the load goes on: a session with one missing file still brings back
the rest.

### 4.3 When the plugins differ

A dialog lists the difference ("V-Ray will be turned on; Arnold will be turned off; Houdini Engine isn't
installed") with:

- **Restart and Load**: sets the plugins' enablement (as the Plugins panel does), then restarts the app
  with `--open-session "<name>"`. Unsaved edits are asked about as closing the window asks.
- **Load Without**: loads as is. Panels of a missing plugin's types stay on the list, as
  `ViewPanelRegistry::savePanels()` already does, so they come back with the plugin.
- **Cancel**.

A plugin that isn't installed only warns; it can't block loading.

### 4.4 Saving

- **Unsaved edits** are not stored, and saving the session doesn't save them. To be sure that's not a
  surprise later, saving warns first when any open stage has unsaved layers:

  > kitchen.usda and props.usda have unsaved edits. The session only remembers the files, so these
  > edits won't be in it.
  >
  > [Save Session Anyway] [Cancel]

  The layers are listed as `confirmUnsavedLayers()` lists them. Saving the layers stays the user's
  choice, through File > Save as usual; the warning doesn't offer it, so it can't save something by
  accident. The automatic Last session on quit (§4.5) doesn't warn: quitting already asked about them.
- Takes the preview **before** the dialog opens (§6.2).
- Overwriting an existing session asks first.

### 4.5 Command line and start

- `usdcc --open-session "Kitchen lookdev"`: start with that session. A stage given too is opened after
  it. (Not `--session`: QApplication takes that for Qt's own session management and removes it.)
- A setting, **Settings > Files > Sessions > On start**: *Empty* (today's behaviour, the default) or *Last
  session*. The last session is saved on quit as `sessions/Last session.json`, shown first in the dialog,
  and can't be deleted from there (it's written again on each quit).

## 5. Files that changed on disk

A session points at files; it doesn't hold them. If a file changed since the session was saved, the
stage opens as the file is now, and the session's state is applied as far as it fits:

- Selected or expanded prims that no longer exist are dropped.
- An edit target layer that's no longer in the stack falls back to the root layer, with a warning.
- A load rule for a missing path is ignored.

Nothing here is an error: it's the normal case after someone else edits the files.

### 5.1 Sessions from someone else

There is no shared folder. A user sends the session's JSON file to another user, who imports it:

- **Export...** in the dialog saves a copy of the selected session wherever the user picks (to attach to
  a mail or a ticket). It's the same file as in the sessions folder.
- **Import...** copies a JSON file into the sessions folder, asking before replacing one of the same
  name. Dropping a `.json` file on the dialog does the same. The file is checked first (`format`,
  `version`); one that isn't a session is refused.
- `usdcc --open-session path/to/file.json`: a path instead of a name loads that file directly, without
  importing it.

The paths in it are the sender's. When files are missing on load, it asks about the first: **Locate...**
or **Skip**. Picking where it is now also maps its folder, so every other missing path under
the same folder is found too (`C:/Users/anna/proj/` → `D:/work/proj/`). The mapping is applied to the
stages, the edit targets, the muted layers and the Script Editor's file tabs, and then saved into the
imported session so it's asked only once.

Paths stay absolute. Both users usually have the project in different places, and a session lives in
AppLocalData, not next to the project, so relative paths wouldn't help.

What doesn't travel:

- **Plugins** the receiver doesn't have: they're named as missing (§4.3) and the session loads without
  them.
- **Plugin tabs** in the Script Editor whose plugin folder doesn't exist: skipped with a warning.
- **Screens** the receiver doesn't have: §5.2.

### 5.2 Different screens

A session saved on one screen setup and loaded on another, say a 4K screen and then a Full HD laptop.
Qt works in scaled pixels, so the difference is often smaller than it looks: 4K at 200% and Full HD at
100% are both 1920×1080 to the app. 4K at 150% (2560×1440) to Full HD at 100% is the real case: three
quarters the width.

- **The window**: at start, `restoreGeometry()` already shrinks a window that's too big and moves one
  that's off screen onto the available screen; a maximised one maximises there. On a load into the
  running app the window doesn't change anyway (§9).
- **View panels**: the ADS splitters store pixels, and Qt's `QSplitter::setSizes()` shares out space
  that's missing or extra in proportion, so a 50/50 split stays 50/50. Panels side by side whose minimum
  widths don't fit the smaller window are squeezed; nothing to do about that beyond what Qt does.
- **Floating panels** keep their saved screen position, so one that floated on the right of a 4K screen or
  on a second monitor can come back off screen. After step 7 of §4.2, each floating panel (an ADS
  `CFloatingDockContainer`, or a floating side panel) that isn't fully on a screen is moved onto the
  nearest one, and shrunk to fit its available area if it's bigger. The same check runs at start for the
  layout from `usdcc.ini`, which has the same problem after unplugging a monitor.
- **Side panels** (Log, Script Editor, Plugins) keep their width in pixels in `QMainWindow::saveState()`,
  so only the middle area shrinks: a Script Editor a quarter of a 2560-wide window is a third of a
  1920-wide one. The session also stores each docked side panel's width or height as a share of the
  window (`sidePanelShares` in `layout`, §3.2). After restoring, if the window differs from the saved one
  by more than 10%, each side panel is resized to its share (`QMainWindow::resizeDocks()`). Within 10%
  pixel sizes are kept, so a session loaded on the same screen comes back exactly as saved.

## 6. The dialog

### 6.1 Layout

File > Sessions... (Ctrl+Alt+S), and File > Open Session > (the five most recent, then "More...").

```
┌ Sessions ──────────────────────────────────────────────────────────┐
│ [Search…                ]                                            │
│ ┌──────────────────────────────┐ ┌─────────────────────────────────┐ │
│ │ ▣ Last session    today 18:02│ │                                 │ │
│ │ ▣ Kitchen lookdev today 14:32│ │      [ preview screenshot ]     │ │
│ │ ▣ Shot 040        3 Nov      │ │                                 │ │
│ │                              │ ├─────────────────────────────────┤ │
│ │                              │ │ Kitchen lookdev                 │ │
│ │                              │ │ Saved today 14:32               │ │
│ │                              │ │ Stages: kitchen.usda, props.usda│ │
│ │                              │ │ Plugins: V-Ray                  │ │
│ │                              │ │ ⚠ props.usda is missing         │ │
│ └──────────────────────────────┘ └─────────────────────────────────┘ │
│ Name: [Kitchen lookdev        ] [Save]                               │
│ [Rename] [Delete] [Import…] [Export…] [Open Folder]   [Load] [Close] │
└──────────────────────────────────────────────────────────────────────┘
```

- **The list**: a thumbnail, the name, when it was saved; newest first. Double-click loads.
- **The details**: a large preview, the stages and plugins, and warnings found by checking the files
  exist (missing stage files, missing plugins) before loading.
- **Save**: the name field starts with the current session's name (the one last loaded or saved), so
  Save again updates it. Typing a new name saves a new one.
- **Delete**: asks, then moves the JSON to the recycle bin (`QFile::moveToTrash()`), not a permanent
  delete.
- **Rename**: renames the file, and the name inside the JSON.
- **Import... / Export...**: §5.1.
- **Open Folder**: shows the sessions folder in Explorer.
- **Previews filled in after**: the list shows at once with a grey thumbnail each, and the previews are
  decoded one per event loop turn and scaled down for the list. The details pane shows the full
  1280-pixel image, scaled to its size.

Like the other dialogs (`PrimTreeDialog`), its size is remembered.

### 6.2 The preview

- **What**: the main window as it looks, scaled to 1280 pixels wide (never up: a smaller window stays
  its size), as PNG. PNG keeps the UI's text sharp. Stored base64 in the JSON (§3.1).
- **How** (`captureWindowPreview()`): `QWidget::grab()` of the window, with each viewport's last frame
  (`HydraViewportWindow::grabFrame()`, its framebuffer object) drawn over its place: `grab()` alone leaves
  the viewports black, as they're native windows of their own. This was the fallback in the plan; it's the
  only way now. Capturing the screen (`QScreen::grabWindow()`) was tried and dropped: by the window's
  handle it leaves out the viewport, the screen's area takes whatever covers the window, and a locked
  screen gives a black picture.
- **When**: before the Sessions dialog opens. Saving from the dialog uses that picture.
- **Last session**: the preview is taken as the window closes (`MainWindow::closing()`).

## 7. Python

```python
usdcc.app.sessions()                      # names, newest first
usdcc.app.save_session("Shot 040")        # overwrites without asking
usdcc.app.load_session("Shot 040")        # returns when done; raises if a restart is needed
usdcc.app.load_session("Shot 040", discard_unsaved=True)  # raises on unsaved edits without it
usdcc.app.delete_session("Shot 040")      # to the recycle bin
usdcc.app.import_session("D:/mail/Shot 040.json")  # returns its name
usdcc.app.export_session("Shot 040", "D:/out/Shot 040.json")
usdcc.app.current_session()               # the name last loaded or saved, or None
usdcc.app.on_session_save(callback, key=None)  # §2.7
usdcc.app.on_session_load(callback, key=None)
```

And on Python view panels, `save_state()` and `restore_state(state)` (§2.4).

## 8. Phases

| Phase | What | Done when |
|---|---|---|
| 0 | `Session`, `SessionStore`: format, file names, list, read, write, rename, delete. No UI. | Unit tests: round trip, odd names, case clash, newer version refused, unknown keys kept. |
| 1 | Capture and load stages, the current stage, the view panels, the layout (§4.2 steps 2–7, 9) and the Script Editor's tabs (§2.5). The unsaved-edits warning (§4.4). File > Save Session... and File > Open Session > as plain menus. Python `save_session`/`load_session`/`sessions`. | Save with two stages, a duplicated viewport and two scratch tabs, load in a fresh app: same stages, same panels in the same places, same scripts. |
| 2 | The Sessions dialog with previews (§6), Import and Export (§5.1, without Locate). Different screens (§5.2): floating panels back on screen, side panels by share. | Previews show viewports, not black. Delete goes to the recycle bin. An exported file imports on another machine. A session saved in a 2560-wide window, loaded into a 1920-wide one, keeps its side panels' proportions and every floating panel on screen. |
| 3 | Per-stage state (selection, edit target, variant edit, muted layers, load rules, session layer) and per-panel state (§2.4), Viewport and Outliner first, then the rest. Python panels' `save_state`/`restore_state`. | Camera, renderer, expansion, unloaded payloads come back. |
| 4 | Plugins: record them, compare on load, Restart and Load, `--open-session` (§4.3, §4.5). | A session saved with a plugin on, loaded with it off, restarts with it on and its panels in place. |
| 5 | Locate... and folder mapping for missing files (§5.1). | A session from another user's paths loads after locating one file. |
| 6 | Last session and the On start setting; plugins' data hooks; in-memory stages stored as text. | Quit and start again brings everything back with the setting on. |

Phases 0 and 1 are useful alone. 2 and 3 are independent of each other; 4 and 5 need 1.

### 8.1 What was built

- **Code**:
  - `usdcc_ui`: `SessionStore` and the `session::` helpers (`session_store.h`), and
    `MainWindow::layoutState()`/`restoreLayoutState()`, `keepFloatingPanelsOnScreen()`, `fitOnScreens()`
    and the `closing()` signal. `ViewPanel::saveSessionState()`/`restoreSessionState()`.
  - `usdcc_usd_ui`: `session_stages` (a stage's state, and reopening it without loading its payloads)
    and `captureWindowPreview()`.
  - `usdcc_scripting_ui`: `SessionController` (capture, save, load, plugins, Locate, Python's
    `SessionHost`) and `SessionsDialog`. The Script Editor's `sessionState()`/`restoreSessionState()`.
  - `main.cpp`: File > Sessions... (Ctrl+Alt+S) and File > Open Session >, `--open-session`, the Last
    session on close, and the restart.
- **Differs from the plan**:
  - `--open-session`, not `--session` (§4.5).
  - The preview is always drawn from the widgets and the viewports' frames (§6.2).
  - Locate asks about one missing file at a time (§5.1), not a list.
  - The Layer Editor and Dependencies panels keep nothing yet (§2.4).
  - There's no File > Save Session item: the dialog saves, and "Last session" is saved on close.
  - Loading also says, before it replaces them, which untitled Script Editor tabs with text aren't in the
    session ("The Script Editor's Untitled 2 is replaced by the session's tabs").
  - Discarded edits are thrown away with `SdfLayer::Reload()` before the stages close, so a file opened
    again comes from disk rather than from the edited layer still in memory.
- **Tests**: `tests/session_store_test.cpp` (ui tests: names, round trip, versions, import and export,
  paths) and `tests/session_test.cpp` (python tests: a full round trip, unsaved edits, moved files,
  plugins that differ, the Python API, the layout and the dialog). `python_panels_test` checks a Python
  panel's `save_state()`/`restore_state()`.
- **Checked in the app** (2026-11-08, a deployed build, a test profile): save, open another stage, load
  back (the extra stage closes, the selection returns); the dialog with its preview; the preview showing
  the viewport; starting with `--open-session "Last session"`; Restart and Load with a session saved
  without the V-Ray plugin (the app restarted with V-Ray off and the session loaded). Not checked in the
  app: side panels scaled on a smaller screen, and floating panels moved back on screen (only
  `fitOnScreens()` is tested).

## 9. Decided

- **Preview**: PNG, 1280 pixels wide, inside the JSON.
- **Sharing**: by sending the JSON file; Import and Export in the dialog, no shared folder.
- **Script Editor**: every tab is in the session, scratch tabs with their text.
- **Unsaved edits**: never stored; saving a session warns about them.
- **Window geometry**: the saved size and position are used only at start (`--open-session`, or On start:
  Last session), when the window is being placed anyway. Loading a session into the running app keeps
  the window where it is and lays the panels out inside it; only a session saved maximised maximises
  the window. Panel sizes scale to the window, so the layout still fits.

## 10. Open questions

1. **A sent session's own file tabs**: a script file the sender had open usually isn't on the receiver's
   machine. Store every file tab's text, not only modified ones, so the receiver at least sees it (as an
   untitled tab when the file is missing)? It makes the file larger, and the text can be out of date.
