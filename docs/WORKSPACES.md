# Workspaces

A workspace is a named, predefined arrangement of view panels for one kind of work: laying out a scene,
shading, editing layers, scripting. Each one is a small JSON file in a folder. A button in the title bar
shows the active workspace and switches to another. Every workspace has a viewport area, which always
holds one viewport per open stage, the current stage's in front.

Also here: **File > Open Stage with Options...**, which opens a stage with a choice of making it current,
switching workspace, and loading its payloads (§8).

Status: **phases 1 to 4 done** (§10.1 says what was built and where it differs from the plan). Planned
2026-11-15 against `680fd27`.

## 1. What there is today

| What | Where | Note |
|---|---|---|
| View panel types, by id (`viewport`, `outliner`, `attributes`, `layers`, `usda`, `materialEditor`, `dependencies`, plus plugins' and Python's) | `ViewPanelRegistry` (`view_panel_registry.h`), filled in `main.cpp` | `create` makes a panel without docking it; `open()` makes and docks one |
| The startup layout: Viewport, Outliner and Attributes as three tabs in one area | `main.cpp` | Their names are fixed (`SessionController::Context::fixedPanels`) |
| A new stage gets a viewport: an empty one, else a new one tabbed beside a viewport | `main.cpp`, `viewportOnStageOpened` | Switched off while a session loads (`sessionLoading`) |
| The layout across restarts, and in sessions | `MainWindow::saveLayout()`, `layoutState()`/`restoreLayoutState()` | ADS XML in pixels, by objectName |
| Stages > a stage > Make Current: every panel on screen shows it, and it becomes current | `StagesMenu::makeCurrentEverywhere()` | |
| Opening a stage off the UI thread | `StageOpener::open(path)` | Always `UsdStage::LoadAll` |
| Opening a stage without its payloads | `StageManager::loadUsdStage(…, LoadNone)` | Used by previews and sessions; Outliner > Load Payload loads them later |
| A list of folders as a setting, with fixed rows | `SettingType::FolderList`, `fixedFolders` | Settings > Plugins > Plugin folders is the model to copy |
| The title bar: logo, menu bar, title, caption buttons | `TitleBar`, registered with QWindowKit in `MainWindow` | Anything clickable in it needs `setHitTestVisible()` |

ADS has named layouts of its own (`CDockManager::addPerspective()`/`openPerspective()`). They aren't used
here: a perspective is the pixel XML state of the panels that existed when it was saved, kept in
`QSettings`. It can't be written by hand, doesn't scale to another screen, and knows nothing of "one
viewport per stage".

## 2. Other DCCs

| App | What it's called | Where you switch | Stored as | Notes |
|---|---|---|---|---|
| **Maya** | Workspaces | A drop-down at the top right of the main window | JSON files in the user's prefs (`workspaces/`) | Ships task workspaces (Maya Classic, Modeling – Standard/Expert, Sculpting, UV Editing, Rigging, Animation, Rendering, …). Changes to a workspace are remembered; *Reset* goes back to the saved one; *Save as New Workspace* |
| **3ds Max** | Workspaces | A drop-down at the right end of the menu bar | Files in the user's settings | *Manage Workspaces* dialog; a workspace also holds menus and toolbars |
| **Houdini** | Desktops | A drop-down at the top of the window | `.desk` text files in the user's pref folder | Build, Animate, Technical, Solaris, … The **Solaris** desktop is the USD one: Scene View, Scene Graph Tree, Scene Graph Details, Network, Parameters. *Save Current Desktop As* |
| **Blender** | Workspaces | Tabs along the top bar | Inside the `.blend` file (defaults from `startup.blend`) | Layout, Modeling, Sculpting, UV Editing, Texture Paint, Shading, Animation, Rendering, Compositing, Geometry Nodes, Scripting. A workspace can also switch the edit mode. Layouts travel with the scene file, which is often unwanted |
| **Nuke** | Workspaces | Workspace menu, and Shift+F-keys | XML files in the user's `.nuke` folder | Compositing, Animation, Large Timeline, … |
| **Omniverse USD Composer** | Layouts | Window > Layout | JSON files | A USD app: Default, Viewport Only, task layouts; quick save and load |
| **Katana** | Layouts | Layouts menu | Files per user | Studios ship layouts with their pipeline |
| **Unreal Editor** | Layouts | Window > Load Layout | INI files | Save Layout As; Default Editor Layout |
| **DaVinci Resolve** | Pages | Tabs along the bottom | Fixed | Media, Cut, Edit, Fusion, Color, Fairlight, Deliver: one per stage of the job, not user-defined |

What they share, and what usdcc takes from it:

1. **The switch sits at the top right** (Maya, 3ds Max, Houdini): out of the way, always visible, and the
   active name is readable at a glance. Blender's tabs take a row of their own. → The title bar button
   (§6), as asked.
2. **Workspaces are files a studio can share** (Maya, Houdini, Nuke, Omniverse). Blender keeping them in
   the scene file is the exception, and the one people work around. → JSON files in folders, with
   studio folders in the settings (§5).
3. **Five to ten workspaces, named after the task.** More becomes a list nobody reads. → Eight built in
   (§4), plus what studios and plugins add.
4. **Tweaks are remembered per workspace, with a Reset** (Maya, Blender). Dragging a panel while in Look
   Dev and coming back to Look Dev later finds it where it was dragged. → **Not in usdcc** (§11 #2):
   choosing a workspace puts the window back as its file says, every time; a layout worth keeping is
   saved as a workspace of its own.
5. **Their files hold the docking state in pixels, written by the app.** Nobody writes a `.desk` file by
   hand. → usdcc's format is a split tree in fractions (§3), short enough to write and review by hand,
   and the same on any screen.
6. **Switching never touches the scene** (Blender's edit mode aside). → Switching changes panels only,
   never stages, selections or edits; the one rule tied to the scene is the viewport area (§7).
7. **Shortcuts to jump straight to one** (Nuke). → A shortcut setting per workspace, empty by default.

## 3. The file format

### 3.1 An example

```json
{
  "format": "usdcc-workspace",
  "version": 1,
  "id": "lookdev",
  "name": "Look Dev",
  "description": "Shade and light: the material graph under the viewport.",
  "group": "Shading & Rendering",
  "order": 40,
  "layout": {
    "split": "horizontal",
    "sizes": [0.18, 0.57, 0.25],
    "items": [
      { "tabs": [{ "panel": "outliner" }] },
      {
        "split": "vertical",
        "sizes": [0.6, 0.4],
        "items": [
          { "tabs": [{ "panel": "viewport" }] },
          { "tabs": [{ "panel": "materialEditor" }] }
        ]
      },
      { "tabs": [{ "panel": "attributes" }, { "panel": "layers" }], "current": 0 }
    ]
  },
  "sidePanels": { "Log": "collapsed" }
}
```

(The shipped Look Dev also gives the Outliner `"maxWidth": 320` and the Attributes `420`, §3.3.)

```
┌──────────┬──────────────────────────┬─────────────┐
│          │ Viewport kitchen │ props │ Attributes  │
│ Outliner │                          │ │ Layers    │
│          │                          │             │
│          ├──────────────────────────┤             │
│          │ Material Editor          │             │
└──────────┴──────────────────────────┴─────────────┘
```

### 3.2 The top level

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | `"usdcc-workspace"` | yes | Tells a workspace from any other JSON in the folder |
| `version` | int | yes | `1`. Raised when an old app would misread a new file. A newer version is refused with a warning; an older one is read as far as it goes (as sessions do) |
| `id` | string `[A-Za-z0-9._-]+` | no | Stable identity: what overrides match on (§5), what `Workspaces/current` and sessions store. Defaults to the file name without `.json`. Plugins should prefix theirs (`org.usdcc.vray-render.lookdev`) |
| `name` | string | yes | Shown on the button and in the menu |
| `description` | string | no | The menu entry's tooltip |
| `group` | string | no | The heading it's listed under in the menu (§6.2), e.g. `"Shading & Rendering"`. Workspaces from any folder that name the same group are listed together, so a studio's or a plugin's workspace can join a built-in group, or start its own. Matched exactly, case included. None: listed without a heading, or under Personal if the file is in your folder (§6.2) |
| `order` | int | no | Order within the menu, lowest first; ties by name. Default 1000. The built-ins use 10, 20, 30, … Also orders the groups: a group sits where its lowest `order` puts it |
| `layout` | node (§3.3) | yes | The dock area's arrangement |
| `sidePanels` | object | no | Side panel objectName (or title) → `"shown"`, `"collapsed"` or `"hidden"`, or an object `{"state": …, "area": "left"/"right"/"top"/"bottom", "size": 0.3}`: the edge it's docked at, and when shown its share of the window's width (left, right) or height (top, bottom). **A side panel not named is hidden**: a workspace has the side panels it names and no others. Missing or `{}`: none. Unknown names are ignored. The names are `Log`, `Script Editor`, `pluginManagerPanel` (Plugins) and those Python panels set |
| `collapsed` | array | no | View panels collapsed to a tab at a window edge, besides the layout's: entries as in `tabs`, with an `"area"` (`"left"`, the default, `"right"`, `"top"` or `"bottom"`). Never viewports. A panel reused for one, or for the layout, moves there. Save Current Layout writes it |
| `fitOnSwitch` | bool, or `"stage"`/`"selection"` | no | Default `false`. When set, switching to the workspace fits **every** viewport it places, reused ones too, as *Fit All Views* does (§3.5.7); `true` means `"selection"`. Off, only viewports the workspace makes are fitted (their `fit`), and reused ones keep their cameras |

Unknown keys are ignored anywhere, so a later version's or a studio tool's additions don't break loading.

### 3.3 Layout nodes

A node is either a **split** or an **area**.

**Split**: children side by side or stacked.

| Key | Type | Meaning |
|---|---|---|
| `split` | `"horizontal"` or `"vertical"` | Horizontal: side by side, left to right. Vertical: stacked, top to bottom |
| `items` | array of nodes, at least one | A split of one is the same as its child |
| `sizes` | array of numbers | Each child's share. Normalised, so `[1, 3]` and `[0.25, 0.75]` are the same. Missing or the wrong length: equal shares |
| `maxWidth`, `maxHeight` | number | As for an area, below: the whole split's limit in its parent |

**Area**: one dock area of tabs.

| Key | Type | Meaning |
|---|---|---|
| `tabs` | array of panel entries, at least one | In tab order |
| `current` | int or panel id | The tab in front. Default: the first; in the viewport area, the current stage's viewport |
| `maxWidth` | number | The most room the area gets across a horizontal split, in pixels at 100% UI scale (scaled with it). Default: no limit |
| `maxHeight` | number | The same across a vertical split |

**Limits.** When any node of a workspace has a `maxWidth` or `maxHeight`, the workspace is laid out by
them rather than by shares alone: in each split, a child with a limit gets its share but no more than the
limit, children without viewports keep their share, and **the room left goes to the viewports, evenly**,
each child getting a part for every viewport it has side by side along the split (a column of two stacked
viewports counts once across a horizontal split, a row of two twice). The children holding viewports are
also the ones that grow and shrink with the window; the others keep their size. A split with no viewports
gives the room left to its children without a limit, by their shares. The user can still drag a divider
past a limit; choosing the workspace again applies it again. A limit on a split nested in
one of the same direction is lost when the two are merged (§3.4).

**Panel entry**:

| Key | Type | Meaning |
|---|---|---|
| `panel` | string | A `ViewPanelRegistry` id: `viewport`, `outliner`, `attributes`, `layers`, `usda`, `materialEditor`, `dependencies`, or a plugin's or Python's |
| `title` | string | The tab's title instead of the usual one |
| `optionsPinned` | bool | Pin the panel's options (`ViewPanel::setOptionsPinned()`) |
| `state` | object | Given to the panel's `restoreSessionState()` when the workspace makes it ([SESSIONS.md](SESSIONS.md) §2.4): a viewport's renderer, an Outliner's type filter, a Python panel's own state. A panel reused from before keeps its own state |
| `stage` | `"each"` or `"current"` | Viewports only. `"each"` (the default): the per-stage entry, one viewport per open stage. `"current"`: one viewport that **follows the current stage** (§3.5.4) |
| `camera` | object | Viewports only: how its camera starts (§3.5) |

### 3.4 Rules

- **Exactly one per-stage viewport entry** (`{"panel": "viewport"}`, `stage` `"each"` or left out). Its
  area is the *viewport area*. On switching, that entry stands for one viewport per open stage, at that
  place in the tab order (§7). A file with none, or with two, is refused with a warning naming the file.
- **Any number of `"stage": "current"` viewports**, anywhere, including in the viewport area.
- **A panel type that isn't registered** (its plugin is off, or a later version of the app dropped it) is
  left out. An area left with no tabs is dropped, and a split left with one child becomes that child. The
  workspace's menu entry says so in its tooltip ("Needs vrayRender, which isn't loaded: left out."), by the
  type's id: the registry doesn't know a type's name once it's gone. Switching to it shows **one warning
  toast**, titled with the workspace's name and naming every type left out, which is its line in the log
  too (`WorkspaceController::applyDefinition()`, [NOTIFICATIONS.md](NOTIFICATIONS.md)); the switch itself
  succeeds. The file isn't changed, so the panel is back with the next switch once its type is registered.
- A type may appear more than once (two Outliners): each entry gets a panel of its own.
- Sizes are shares of the parent split, not pixels; panels' own minimum sizes still apply.
- A file that doesn't parse, or breaks a rule, isn't listed. The log says which file and why; a studio
  that ships a broken file sees it as soon as someone opens the menu.

A JSON Schema (`resources/workspaces/workspace.schema.json`) ships next to the built-ins, so editors
check the files as they're written. The app doesn't need it to read them.

### 3.5 Viewport cameras

A workspace file is written once and used with any stage, so it can't hold camera positions: a camera
8 units from the origin suits a chair and is inside a building. What it holds is **the view**: which way
the camera looks, the projection, and how to frame what's there. Positions come from the stage when the
camera is applied.

#### 3.5.1 The `camera` object

| Key | Type | Default | Meaning |
|---|---|---|---|
| `view` | `persp`, `top`, `bottom`, `front`, `back`, `right`, `left` | `persp` | Which way it looks (§3.5.2) |
| `projection` | `perspective` or `orthographic` | `orthographic` for the six axis views, `perspective` for `persp` | `"top"` with `"perspective"` looks down from above in perspective |
| `fit` | `stage`, `selection` or `none`; `true` is `stage`, `false` is `none` | `stage` | Fit to View once the viewport has its stage: the whole stage's bounds, the selection (else the whole stage, as F does), or leave the camera where the rest puts it |
| `yaw`, `pitch` | degrees | the view's | Turn the view, about the stage's up axis (§3.5.2); mostly for `persp` (`{"view": "persp", "yaw": 135, "pitch": 30}` looks from behind) |
| `fov` | degrees | the app's | A perspective camera's vertical field of view (`setFieldOfView()`) |
| `target`, `distance` / `size` | `[x, y, z]`, number | | Exact placement in scene units: the orbit target, and the perspective distance or the orthographic view's height. Giving either means `fit` `none` unless set. For workspaces made for one show's scale |
| `path` | prim path, or `"first"` | | Look through a `UsdGeomCamera` of the stage (§3.5.5), `"first"` being the first one found. Where there's none, the rest of the object applies |

Applied when the workspace **makes** the viewport. A viewport reused from before keeps its camera; it's
picked for the entry only if its view and projection match (§7.1 step 3), so a Top view is never reused
as the perspective one.

The tab's title defaults to the view's name ("Top", "Front", "Right", ...) for any view but `persp`, and
the HUD's camera label shows it in place of "Persp" ([VIEWPORT_HUD.md](VIEWPORT_HUD.md) §2.1).

#### 3.5.2 The views and the up axis

The views are named, as in Maya, by where the eye is. They follow the stage's up axis
(`UsdGeomGetStageUpAxis`), as the grid and the axis triad do already, so *Top* always looks down at the
ground:

| View | Eye on, Y-up stage | Eye on, Z-up stage |
|---|---|---|
| `persp` | the default yaw 45, pitch 20 | the same |
| `front` | +Z | −Y |
| `back` | −Z | +Y |
| `right` | +X | +X |
| `left` | −X | −X |
| `top` | +Y | +Z |
| `bottom` | −Y | −Z |

The orbit camera turns about the stage's up axis: `yaw` goes around it and `pitch` is the height above
the ground, XZ on a Y-up stage and XY on a Z-up one (`orbitCameraAxes()`). A Z-up stage's camera is the
Y-up one turned a quarter about X, Y onto Z, which is where the Z-up column of the table comes from. So:

- A stage stands upright on its grid whichever its up axis, in the default view and while orbiting.
- The same `yaw` and `pitch` look at the ground the same way on either. A view has the same angles on
  both (`orbitAnglesForView()`): Top is pitch 90, yaw 0.
- The side views have the up axis up the screen. Top has X to the right, and −Z up the screen on a Y-up
  stage, +Y on a Z-up one.
- A view stays the same view when the viewport switches to a stage with the other up axis, or when the
  stage's up axis is changed (Layer Properties).

The names are the ones the triad's tooltips give (`hudAxisViewName()`). The Material Editor's preview is
the exception: its camera is always Y-up ([MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) §7.3).

#### 3.5.3 Navigating an orthographic view

- Pan and zoom as in perspective; zoom changes the view's height (`size`), not a distance.
- **No tumbling on an axis view**: the drag that orbits does nothing, so a Top view stays a Top view
  (Maya's default). Clicking the axis triad still switches it to another axis view.
- The Persp view switched to Orthographic (the HUD's "Ortho") tumbles as in perspective, and turning
  it keeps it orthographic. Switching Orthographic on or off keeps the view's angles.
- Fit to View (F) fits the box's extent across the view, not its bounding sphere.
- The grid, picking and the gizmos work from the frustum, which is orthographic now; the render view
  panels already accept an orthographic frustum (`render_view_panel.cpp`).

#### 3.5.4 Viewports that follow the current stage

A four-view needs four viewports on **one** stage. The per-stage entry can't do that: it is one viewport
per stage. So the extra views are `"stage": "current"` viewports:

- They show the current stage, and **switch with it**: clicking another stage's tab in the viewport area
  makes it current (as now), and every following view goes with it, refitted if its `fit` isn't `none`.
- **Follow Current Stage**, the first entry of every view panel's Stage dropdown, starts it; picking a
  stage there stops it (`ViewPanel::setFollowsCurrentStage()`). Any view panel can follow, not only
  viewports. While following, the entry names the stage it shows: *Follow Current Stage (shot.usda)*.
- Settings > Appearance > **Follow Current Stage** has a check box per panel type
  (`Appearance/followCurrentStage.<id>`, defined by `ViewPanelRegistry::add()`). On, every panel of the
  type follows: each one made from then on, and all open ones at once; off stops them all. A stage's own
  viewport in a workspace (`"stage": "each"`) still shows its stage, and with viewports following, a stage
  opened shows in a following viewport instead of getting a new one.
- With no stage open they're empty, like any viewport.
- Sessions store the following (`"follows": true` on the panel's entry).

#### 3.5.5 Looking through a scene camera

`"path"` needs the viewport to render through a stage's `UsdGeomCamera`, which it can't yet (the HUD
plan lists it, [VIEWPORT_HUD.md](VIEWPORT_HUD.md) §2.1). Once it can, a *Review* workspace can show the
shot camera: `{"path": "first", "view": "persp"}`, falling back to a framed perspective view on a stage
without one. Until then the key is read and ignored, so files written for later still load.

**Done** ([VIEWPORT_CAMERA.md](VIEWPORT_CAMERA.md) §3): a viewport the workspace makes looks through the camera when the
stage has it, and isn't fitted; a following view does so for each stage it's given. Saving a workspace writes the
`path` of a viewport looking through a camera.

#### 3.5.6 A four-view workspace

```json
{
  "format": "usdcc-workspace",
  "version": 1,
  "id": "four-view",
  "name": "Four View",
  "description": "Top, front and right views beside the perspective one.",
  "group": "Scene",
  "order": 20,
  "fitOnSwitch": "stage",
  "layout": {
    "split": "horizontal",
    "sizes": [0.15, 0.85],
    "items": [
      { "tabs": [{ "panel": "outliner" }] },
      {
        "split": "vertical",
        "items": [
          {
            "split": "horizontal",
            "items": [
              { "tabs": [{ "panel": "viewport", "stage": "current", "camera": { "view": "top" } }] },
              { "tabs": [{ "panel": "viewport", "camera": { "view": "persp" } }] }
            ]
          },
          {
            "split": "horizontal",
            "items": [
              { "tabs": [{ "panel": "viewport", "stage": "current", "camera": { "view": "front" } }] },
              { "tabs": [{ "panel": "viewport", "stage": "current", "camera": { "view": "right" } }] }
            ]
          }
        ]
      }
    ]
  }
}
```

```
┌──────────┬──────────────┬──────────────────────┐
│          │ Top          │ Viewport kitchen│props│
│ Outliner │  (ortho)     │  (persp, per stage)  │
│          ├──────────────┼──────────────────────┤
│          │ Front        │ Right                │
│          │  (ortho)     │  (ortho)             │
└──────────┴──────────────┴──────────────────────┘
```

The perspective quadrant is the viewport area, with a tab per stage; the three axis views follow
whichever is current. Maximizing a quadrant (double-click its tab, `PanelMaximizer`) and restoring it is
Maya's spacebar between one view and four.

The two rows are separate splitters, so dragging the divider between Top and the perspective view
doesn't move the one between Front and Right. ADS has no linked cross; equal `sizes` line them up to
begin with (§11 #6).

#### 3.5.7 Fit All Views

With four views, F (Fit to View) fits only the active one. **Fit All Views** fits every viewport on
screen at once (open, its tab in front), each keeping its own view and projection: the selection if
there is one, else the whole stage, as F does, each in its own stage.

- **Shift+F**, as a Shortcut setting (`Shortcuts/viewport.fitAllViews`), an application-wide action on
  the main window like Fit to View (`ViewportViewPanel::addFitAllViewsAction()`), so it works with the
  focus in any panel.
- In the HUD's Camera menu, under Fit to View.
- In the workspace menu (§6.2), under the list: *Fit All Views*, for a mouse user who has just switched.
- Python: `usdcc.app.fit_all_views(selection=True)`.
- Not an undo step, as Fit to View isn't.

A workspace can do it on switching with `fitOnSwitch` (§3.2), for one where every view should start
fitted however it was left; Four View sets it to `"stage"`.

## 4. Built-in workspaces (recommended)

Eight, in `resources/workspaces/`. **Default** is the one a first start gets. It was named
Layout, with the id `layout`: that id still means it (§6.3).

| Group | Order | id | Name | Arrangement | For |
|---|---|---|---|---|---|
| Scene | 10 | `default` | Default | Outliner (18%) │ **viewports** (57%) │ Attributes (25%) | General scene work; the first start |
| Scene | 15 | `side-by-side` | Side by Side | Outliner │ **viewports** │ Viewport 2 │ Attributes (below) | Two angles on the current stage at once: placing something while seeing it from behind |
| Scene | 20 | `four-view` | Four View | Outliner │ Top, **viewports** (persp) over Front, Right (§3.5.6) | Placing and aligning; needs orthographic views (phase 2b) |
| Scene | 30 | `viewport` | Viewport Only | **viewports**; Log, Script Editor and Plugins `collapsed` | Review, screenshots, a small screen |
| Shading & Rendering | 40 | `lookdev` | Look Dev | Outliner │ **viewports** over Material Editor (60/40) │ Attributes, Layers as a tab | Materials and lights (§3.1) |
| USD | 50 | `layering` | Layering | Outliner │ **viewports** │ Layers over Dependencies, USDA as a tab beside Dependencies | Composition: edit targets, sublayers, references, the text of a layer |
| USD | 60 | `inspect` | Inspect | Outliner over Attributes (55/45) │ **viewports** over USDA (65/35) │ Schemas ([SCHEMAS.md](SCHEMAS.md) §4) | Reading a stage someone else built: structure, values, text and the schemas it uses at once |
| Development | 70 | `scripting` | Scripting | Outliner │ **viewports** │ Attributes; Script Editor and Log `shown` | Like Blender's Scripting: the editor beside the scene |

**Side by Side**:

```json
{
  "format": "usdcc-workspace",
  "version": 1,
  "id": "side-by-side",
  "name": "Side by Side",
  "description": "Two perspective views of the current stage, from opposite sides.",
  "group": "Scene",
  "order": 15,
  "layout": {
    "split": "horizontal",
    "sizes": [0.15, 0.34, 0.34, 0.17],
    "items": [
      { "tabs": [{ "panel": "outliner" }] },
      { "tabs": [{ "panel": "viewport" }] },
      { "tabs": [{ "panel": "viewport", "stage": "current", "title": "Viewport 2",
                  "camera": { "view": "persp", "yaw": 225, "pitch": 20 } }] },
      { "tabs": [{ "panel": "attributes" }] }
    ]
  }
}
```

```
┌──────────┬───────────────────────┬──────────────┬────────────┐
│          │ Viewport kitchen│props│ Viewport 2   │            │
│ Outliner │  (persp, per stage)   │  (persp, the │ Attributes │
│          │                       │   other side)│            │
└──────────┴───────────────────────┴──────────────┴────────────┘
```

- **Viewport 1** is the viewport area: a tab per stage, the current stage's in front, as in every
  workspace.
- **Viewport 2** follows the current stage (§3.5.4): clicking another stage's tab in Viewport 1 switches
  it too. Its camera starts in perspective from the opposite side (yaw 225 against the default 45),
  fitted to the stage; after that it's navigated on its own.
- Selection is the stage's, so selecting in either viewport, or the Outliner, shows in both, and the
  gizmo works in both.
- Needs following viewports (phase 2b), but not orthographic views.

Plugins add their own (§5): the V-Ray and RTX plugins could each ship a *Render* workspace (order 45,
group *Shading & Rendering*), viewports and their render view side by side with Attributes on the right,
so it lands under Look Dev. The built-ins don't name plugin panel types, so they look the same with every
plugin off.

Today's startup layout (three tabs in one area) isn't kept as a workspace: it shows one panel at a time,
which is what workspaces are there to fix. A profile that has a saved layout keeps it until a workspace is
chosen (§6.3).

## 5. Where workspaces come from

Looked in from the top; **the first file holding an id wins**, as with plugins
([PLUGINS.md](PLUGINS.md) §1). The others are ignored, with an info line in the log.

1. **Your workspaces**: `<AppLocalData>/workspaces/` (next to `sessions/`). *Save Current Layout as
   Workspace...* writes here.
2. **Settings > Files > Workspaces > Workspace folders**, in their order: a studio's shared folder, a
   project's.
3. **`USDCC_WORKSPACE_PATH`**: folders separated as `PATH` is, for pipelines that set up the app from a
   wrapper.
4. **Plugins' `workspaces/` folders**, in plugin order. Just a folder of that name in the plugin; nothing
   in the manifest.
5. **Shipped with usdcc**: `<install>/resources/workspaces/`.

So a user can change a shipped workspace by saving one with its id, and a studio can replace Default for
everyone. The menu then offers *Reset to "<source>"* on a workspace that shadows another (§6.2).

The setting (`Workspaces/folders`):

```cpp
.key = "Workspaces/folders", .category = "Files", .group = "Workspaces",
.label = "Workspace folders",
.description = "Folders of workspace files (.json), looked in from the top: the first holding a "
               "workspace's id wins. Your workspaces and the ones shipped with usdcc are always looked in.",
.type = SettingType::FolderList, .defaultValue = std::string(),
.fixedFolders = {{.label = "Your workspaces", .folder = <user folder>},
                 {.label = "Shipped with usdcc", .folder = <shipped folder>, .after = true}}
```

It applies at once: changing it reads the folders again. They're read at start, when the setting
changes, after saving, and from *Reload Workspaces* in the menu; **not** each time the menu opens, as a
studio folder on a share that's offline would hang it (File > Open Recent avoids probing for the same
reason, [SETTINGS.md](SETTINGS.md) §4).

## 6. The title bar button

### 6.1 Look

```
┌─[◆] File Edit Stages View Help ───────── usdcc — kitchen.usda ───────── [▦ Look Dev ▾] [–] [□] [×]┐
```

- A `QToolButton#titleBarWorkspaceButton`, right of the title and left of the minimize button: **a push
  button** with the workspace's name and a down arrow on its right, `Qt::ToolButtonTextOnly`, popup mode
  `InstantPopup`, 22 pixels high (scaled) rather than the title bar's height.
- **On the accent fill**, as a dialog's main button. In `app.qss`: `background: @accent`, white bold text,
  3px corners, hover `@accent-hover`, pressed or open 75% of the accent; the arrow is the menu indicator,
  `chevron-down-white.png` (`tools/gen_theme_icons.py`).
- The name is elided past about 160 pixels (scaled); below a narrow window width only the icon shows,
  with the name in the tooltip.
- `TitleBar` gets a generic `addTrailingWidget(QWidget*)`, so it keeps knowing nothing of workspaces;
  `MainWindow` registers the widget with `m_windowAgent->setHitTestVisible()`, or clicks on it would drag
  the window.
- On macOS the button is the last thing in the bar, as the caption buttons are hidden there.

### 6.2 The menu

```
  SCENE ────────────────
✓ Default
  Side by Side
  Four View
  Viewport Only
  SHADING & RENDERING ──
  Look Dev
  V-Ray Render                  ← a plugin's, joining a built-in group (order 45)
  Studio Lighting               ← a studio folder's, joining it too (order 48)
  USD ──────────────────
  Layering
  Inspect
  DEVELOPMENT ──────────
  Scripting
  ACME PIPELINE ────────        ← a studio's own group (order 500)
  Shot Review
  PERSONAL ─────────────        ← yours that name no group (order 900 when saved)
  My Layout
  ─────────────
  Fit All Views                 Shift+F
  ─────────────
  Reset to "Shipped with usdcc"          only on one that shadows another
  Save Current Layout as Workspace...
  ─────────────
  Open Personal Workspace Folder         your folder (§5, item 1)
  Reload Workspaces
  Workspace Folders...                   Settings > Files, on the setting's row
```

- **Grouped** by `group`, each group under a heading. Groups are ordered by their lowest `order`, and the
  workspaces in a group by `order`, then name. Workspaces with no group, other than yours (below), form a group without a heading,
  placed by their lowest `order` like any other. With only one group (or none) there are no headings.
  A check on the active one.
- **Personal:** the workspaces in your folder (§5, item 1) that name no group, which is what *Save
  Current Layout as Workspace...* writes, are listed under a **Personal** heading
  (`WorkspaceLibrary::menuGroup()`), placed by their lowest `order` as a group is. The heading isn't
  written to the file, and `usdcc.app.workspaces()` still gives their `group` as empty. Your copy of a
  workspace that has a group stays in that group, where the one it replaces was; a file that names the
  group `Personal` itself joins the heading.
- Headings are section labels, not submenus: every workspace stays one click away. They're
  `QWidgetAction` labels (`#workspaceMenuHeading` in `app.qss`, small caps in the dimmed text color),
  since `QMenu::addSection()` draws its text or not depending on the style.
- The Workspace list of Open Stage with Options (§8.2) groups the same way, with the headings as
  disabled items.
- `usdcc.app.workspaces()` gives each one's `group`.
- Each entry's tooltip: the description, then where its file is.
- A workspace that needs a missing panel type is listed, its tooltip saying what's missing; choosing it
  lays it out without that panel and says so in a toast (§3.4).
- **The same menu is View > Workspace**, the View menu's first entry, for a window whose title bar is out of
  reach (full screen) or for the keyboard. And a view panel's tab menu has it as **Workspace**, right
  under Close (and Close Others) (`ViewPanel::setTabWorkspaceMenu()`, set in main.cpp).
- **Stages > a stage > Workspace**, above Views: the workspaces alone (no commands below them), each laid
  out with that stage made current first, so its viewport is in front and the other panels show it. The
  check is on the active workspace only in the current stage's submenu. Choosing the active workspace lays
  it out again as it is now, with that stage current.

### 6.3 Remembering

- **The active workspace** is UI state: `Workspaces/current` in `usdcc.ini`.
- **At start** the saved dock layout is restored as today, and the button shows `Workspaces/current`. On a
  profile with no saved layout (the first start), **Default** is applied once the window has settled
  (`MainWindow::layoutSettled()`), instead of today's three tabs.
- **How each was left is not kept.** Choosing a workspace, the active one too, lays it out as its file
  says: it's how the window's layout is put back after panels were moved, opened or closed. To keep a
  layout, *Save Current Layout as Workspace...*. The `Workspaces/arranged/<id>` keys an earlier version
  wrote are removed at start.
- **Default's id was `layout`**, and what says so still means Default (`workspace::renamedId()`):
  - a workspace file with that id, or named `layout.json` with none, is read as `default`, so your copy
    or a studio's still replaces the shipped one. Saving over your copy writes the file it is in;
  - `Workspaces/current` and `Shortcuts/workspace.layout` in `usdcc.ini` are rewritten at start to
    `default` and `Shortcuts/workspace.default`;
  - a session's `"workspace": "layout"` and `usdcc.app.set_workspace("layout")` give Default.
    `current_workspace()` and `workspaces()` say `default`.

  So `layout` can't be the id of another workspace.
- **Sessions** store the active workspace's id in `layout` (`"workspace": "lookdev"`). Loading one
  restores its exact layout, as now, and sets the button. The session format's `version` doesn't change:
  older apps ignore the key.

## 7. Switching

### 7.1 What happens

Given workspace *W* and the open stages *S₁…Sₙ*, with *C* the current one:

1. **Check** that the panels about to close can close. `ViewPanel` gets `virtual bool canClose()` (true by
   default), for a panel that holds something not yet applied. If one says no, nothing changes.
2. **Leave a maximized panel** (`PanelMaximizer`): its restore puts the layout back first.
3. **Pick the viewports**: for each stage, in open order, the viewport already showing it, preferring
   one on screen, then the last focused. A stage with none gets a new one from the registry. With no
   stage open, one viewport with none (an existing empty one if there is one). Then each
   `"stage": "current"` entry, in the file's order. A viewport is only reused for an entry whose camera
   view and projection it has (§3.5.1); a perspective viewport for the per-stage entry. A new one gets the
   entry's `camera`, framed once it has its stage.
4. **Pick the other panels**: for each entry, in the file's order, an unused existing panel of that type
   (preferring one on screen and the fixed Viewport/Outliner/Attributes), else a new one from the
   registry. Each is switched to *C*, as Make Current does: a workspace shows the stage you're working on.
   New ones get the entry's `title`, `optionsPinned` and `state`.
5. **Close the rest**: every view panel not picked. The ones the registry made are deleted, so they don't
   pile up hidden; the three fixed ones are closed (hidden), as now, and are the first reused next time.
   Floating panels count too.
6. **Build the tree** (§7.2), with the window's updates off so it appears in one step.
7. **Tabs in front**: the viewport area shows *C*'s viewport; the others their `current`.
8. **Focus** *C*'s viewport (`setDockWidgetFocused()`), so `CurrentStageTracker` keeps *C* current.
9. **Side panels** to their `sidePanels` state, edge and size; every side panel not named is hidden. Done
   before the sizes of step 6's tree, since side panels take their room from the dock area.
10. Save `Workspaces/current`; the button follows.

Switching isn't an undo step and asks nothing (step 1 aside): it changes panels, not stages. What it does
lose is the view state of the panels it closes (a duplicated viewport's camera, a USDA panel's file),
which is why it reuses before it makes.

**Duplicate viewports** of one stage: only one stays (step 3); the others close. The viewport area holds
exactly one per stage.

### 7.2 Building the tree with ADS

Through ADS's public API, not by writing its XML state:

1. Take every picked panel out of its place (`removeDockWidget()`). A closed one is opened where it is
   first: ADS 4.5 inserts a closed dock widget with its tab hidden and then marks it open
   (`CDockAreaWidget::insertDockWidget()`), so a viewport closed before switching back came back
   without its tab, and still unticked in the View menu (fixed 2026-11-18).
2. Walk the tree breadth first. The root's first area goes in the centre
   (`addDockWidget(CenterDockWidgetArea, …)`); each later child of a split goes beside the previous
   child's first area (`RightDockWidgetArea` for horizontal, `BottomDockWidgetArea` for vertical). ADS
   puts it in the same splitter when the direction matches, and nests a new one when it doesn't, which is
   what a split inside a split needs. Each area's other tabs go in with `addDockWidgetTabToArea()`.
3. Sizes: once the tree is built and laid out, `setSplitterSizes()` on each splitter, the shares times
   the splitter's size.

Generating ADS's XML and calling `restoreState()` was the alternative. It would apply in one call, but the
format is internal to ADS and changes between its versions. Moving viewports between areas already
happens when a user drags one, so step 1 takes the path the native viewport windows already survive; a
test checks that a moved viewport still draws.

### 7.3 Stages opened and closed afterwards

The viewport area is a rule of the workspace, not only of the switch:

- **A stage opened** gets its viewport as a tab in the viewport area (the area holding most viewports, if
  the user has moved them around), instead of "beside any viewport" as now. This replaces the "new
  viewport" branch of `viewportOnStageOpened`.
- **A stage closed** closes its viewport there, as now; the last one left with no stage stays, empty.

## 8. File > Open Stage with Options...

### 8.1 Menu

Under **Open Stage...**:

```
File
  Open Stage...                    Ctrl+O
  Open Stage with Options...       Ctrl+Shift+O
  Open Recent                    ▸
```

Both shortcuts as Shortcut settings (`Shortcuts/file.openStage`, `Shortcuts/file.openStageWithOptions`),
so they can be changed; neither has a key today.

### 8.2 The dialog

First the usual file dialog. Then, for the chosen file:

```
┌ Open Stage ───────────────────────────────────────────┐
│ kitchen.usda                                           │
│ C:/proj/kitchen/kitchen.usda                           │
│                                                        │
│ [x] Make current                                       │
│     Show it in every panel on screen, and make it the  │
│     current stage                                      │
│                                                        │
│ Workspace: [ Use existing            ▾ ]               │
│                                                        │
│ [x] Load payloads                                      │
│     Off: payloads stay unloaded; load them later from  │
│     the Outliner (Load Payload)                        │
│                                                        │
│                                     [Open]  [Cancel]   │
└────────────────────────────────────────────────────────┘
```

| Option | Default | On | Off |
|---|---|---|---|
| **Make current** | on | Like Stages > Make Current: every panel on screen shows the new stage, its viewport in front and focused, and it becomes current | It gets its viewport as a tab in the viewport area, behind; the current stage, the focus and every other panel stay as they are |
| **Workspace** | Use existing | A workspace: applied once the stage is open, so the new stage's viewport is one of the viewport area's | Use existing: the layout stays; the stage's viewport goes in the viewport area (§7.3) |
| **Load payloads** | on | `UsdStage::LoadAll`, as Open Stage does | `UsdStage::LoadNone`: quicker for a heavy stage; the Outliner's Load Payload brings them in |

- The defaults are the same every time, not the last ones used.
- For comparison, plain **Open Stage** keeps today's behaviour: its viewport in front and current, the
  other panels unchanged, payloads loaded.
- **A file that's open already** comes forward as now. Make current and Workspace still apply; Load
  payloads can't, and an info toast says the stage's payloads are as they were.
- **Python**: the call scripts use to open a stage gains `load_payloads=True`, `make_current=None` and
  `workspace=None` keywords; `None` keeps today's behaviour.

### 8.3 Code

- `StageOpener::Options { bool loadPayloads = true; MakeCurrent makeCurrent = Default; QString workspace; }`,
  `open(path, options)`; the worker passes `loadPayloads` to `loadUsdStage()`.
- While it adopts the stage, `StageOpener` exposes the options (`StageOpener::pendingOptions()`, as
  `isSwitchingStage()` is checked now), so `viewportOnStageOpened` in `main.cpp` knows whether to bring
  the viewport forward and focus it.
- `makeCurrentEverywhere()` moves out of `StagesMenu` into a free function both use.
- `OpenStageOptionsDialog` (`usdcc_usd_ui`), its Workspace list from the workspace library.

## 9. Code

| Where | What |
|---|---|
| `usdcc_ui`: `workspace.h` | `WorkspaceDefinition` (the §3 tree), `fromJson()` with the §3.4 checks and their messages, `toJson()` |
| `usdcc_ui`: `workspace_library.h` | `WorkspaceLibrary`: the folders (§5), scanning, precedence, `list()`, `find(id)`, `shadowed(id)`, `save()`, `remove()` (to the recycle bin), the setting, a `changed` signal |
| `usdcc_ui`: `workspace_applier.h` | `applyWorkspace(definition, Context)`: §7.1 and §7.2. The context gives the dock manager, the registry, the stages in order and the current one, and how to make a viewport; so it's testable without `StageManager`'s USD side, as `CurrentStageTracker` is |
| `usdcc_ui`: `workspace_capture.h` | Phase 4: the dock manager's tree → a `WorkspaceDefinition` (walk `CDockContainerWidget`'s splitters, `splitterSizes()` to shares); the area with the most viewports becomes the viewport area; floating panels are left out. A viewport is written with its `view` and `projection` and, if it follows, `"stage": "current"`; never its position, which belongs to the scene |
| `usdcc_usd_ui`: `HydraViewportWindow` | Phase 2b: `CameraState` gains `view`, `projection` and `size`; an orthographic frustum; no tumbling while orthographic; Fit to View by extent for orthographic |
| `usdcc_usd_ui`: `orbit_camera.h` | Phase 2b: `orbitAnglesForView(view)` (§3.5.2) and orthographic framing beside `frameOrbitCamera()`, Qt-free and unit tested |
| `usdcc_usd_ui`: `ViewportViewPanel` | Phase 2b: `applyCameraSpec(QJsonObject)` for §3.5.1; following the current stage (§3.5.4) and its dropdown entry; `view`, `projection`, `size` and `follows` in `saveSessionState()`; the HUD's camera label |
| `usdcc_ui`: `TitleBar` | `addTrailingWidget()` |
| `usdcc_ui`: `MainWindow` | Registers the trailing widget for hit testing |
| `usdcc_ui`: `ViewPanel` | `virtual bool canClose()` |
| `usdcc_ui`: `WorkspaceButton` | The button and its menu (§6), knowing only the library and a "switch to" callback |
| `usdcc_usd_ui` | `OpenStageOptionsDialog`; `StageOpener::Options`; `makeCurrentEverywhere()` as a free function |
| `main.cpp` | The library, the button, the first-start default, `viewportOnStageOpened` putting viewports in the viewport area, File > Open Stage with Options... |
| `SessionController` | `"workspace"` in `layout` |
| Python (`usdcc.app`) | §9.1 |
| `resources/workspaces/` | The eight files (§4) and `workspace.schema.json`; the deploy copies them |
| `app.qss` | `#titleBarWorkspaceButton` |

### 9.1 Python

```python
usdcc.app.workspaces()                    # [{"id", "name", "group", "description", "file", "missing": [...]}], in menu order
usdcc.app.current_workspace()             # "lookdev"
usdcc.app.set_workspace("lookdev")        # switches; KeyError for an unknown id, False if a panel refused to close
usdcc.app.save_workspace("My Layout", side_panels=True)  # the current arrangement, to your folder; returns its id
usdcc.app.reload_workspaces()
usdcc.app.fit_all_views(selection=True)   # Fit All Views; selection=False fits the whole stage
```

## 10. Phases

| Phase | What | Done when |
|---|---|---|
| 1 | `WorkspaceDefinition`, `WorkspaceLibrary`, the folders setting, the eight shipped files and the schema. No UI | Unit tests: parse and check every §3.4 rule with its message; precedence across folders; unknown types dropped and empty areas collapsed; a newer version refused; unknown keys ignored |
| 2 | Switching (§7.1, §7.2, §7.3), the title bar button and menu (§6.1, §6.2 without Reset and Save), `Workspaces/current`, the first-start default, Python's `workspaces`/`current_workspace`/`set_workspace`/`reload_workspaces` | Two stages open, switch to Look Dev: Outliner, two viewports tabbed with the current stage's in front, Material Editor below, Attributes and Layers right, every other panel closed; the same panels reused when switching back, cameras kept; a third stage opened afterwards lands in the viewport area. Checked in the app: the button's accent color, its clicks not dragging the window, viewports drawing after a switch |
| 2b | Viewport views (§3.5): orthographic projection, the view presets, fitting, following the current stage, `camera` in workspace files, Fit All Views and `fitOnSwitch` (§3.5.7), the Four View and Side by Side workspaces | Unit tests: each view's yaw and pitch, orthographic framing of a box. Four View on a Y-up and a Z-up stage: Top looks down at the ground in both; the three axis views follow when another stage's tab is clicked; picking and the move gizmo work in Top. Checked in the app: no tumbling in an orthographic view; the triad switches it |
| 3 | Open Stage with Options (§8). Independent of 1 and 2 except the Workspace list, which can come with phase 2 | With Load payloads off a payloaded prim is unloaded and Load Payload loads it; Make current off leaves the current stage and the Outliner's stage alone |
| 4 | Save Current Layout as Workspace (`workspace_capture`), Reset to source, a shortcut setting per workspace (`Shortcuts/workspace.<id>`, empty), plugins' `workspaces/` folders, sessions storing the workspace | Saving and applying a layout gives the same tree, sizes within a few pixels |

Phases 1 and 3 can be done at the same time; 2 needs 1; 4 needs 2. Phase 2b's viewport half
(orthographic, presets, following) needs nothing else and can start with phase 1; reading `camera` from a
workspace needs 2.

### 10.1 What was built

All four phases and 2b, on 2026-11-15.

- **Code**:
  - `usdcc_ui`: `workspace.h` (the format, its rules, `pruned()`), `WorkspaceLibrary` (folders, precedence,
    groups, save, remove), `workspace_applier.h` (`apply()`, `capture()`, `viewportArea()`) and
    `WorkspaceController` (the active workspace, the title bar button and its menu, a
    shortcut setting per workspace). `ViewPanel` follows the current stage and has the workspace hooks
    (`canClose()`, `applyWorkspaceCamera()`, `workspaceView()`, `workspaceCamera()`, `fitView()`).
    `ViewPanelRegistry::create()`; `TitleBar::addTrailingWidget()`, `MainWindow::addTitleBarWidget()` and
    `sidePanels()`.
  - `usdcc_usd`: the named views and orthographic framing in `orbit_camera.h`.
  - `usdcc_usd_ui`: orthographic projection and `setView()` in `HydraViewportWindow`; the viewport's
    workspace camera, quiet fitting, Fit All Views, Reset Camera keeping the view, and the HUD's Camera menu
    listing the views and an Orthographic switch; `OpenStageOptionsDialog`; `StageOpener::Options` and its
    `opened()` signal; `makeCurrentEverywhere()` as a free function.
  - `usdcc_scripting`: the Python calls (§9.1), the session's `layout.workspace` and panels' `follows`.
  - `main.cpp`: the library and controller, the button, File > Open Stage with Options... (Ctrl+Shift+O;
    Open Stage got Ctrl+O, both settings under Shortcuts > File), new viewports in the viewport area,
    plugins' `workspaces/` folders, the first-start default, and how the workspace was left on quitting.
  - `resources/workspaces/`: the eight workspaces and the schema; `tools/deploy.py` copies them.
- **Differs from the plan**:
  - Follow Current Stage is the Stage dropdown's first entry, as planned, and a setting per panel type (§3.5.4).
  - Fit All Views isn't in the View menu or the options menu: it's in the workspace menu and the HUD's
    Camera menu, and on Shift+F.
  - A switch reuses the panel on screen before a fixed one behind it: with an Outliner from the Stages menu
    in front, that one stays and the fixed Outliner closes (kept, not deleted).
  - An empty viewport (the app's first, before a stage was opened) is reused for a stage that has none.
  - A view label for a perspective camera on an axis view reads "Top (Persp)"; an orthographic one that
    isn't on an axis view, "Ortho".
  - Saving a workspace whose name matches another's keeps that one's id, group and order, so it replaces
    it (yours shadows it if it came from another folder). A new one gets order 900 and no group, so it's listed under Personal.
  - How a workspace was left is remembered only once one has been chosen (switched to, loaded with a
    session, or chosen in an earlier run): before that the button says Default, but the layout on screen is
    whatever the profile had, which isn't how Default was left.
  - After laying out, the current stage's viewport is focused again once the event loop has run, and the
    current stage put back: moving the panels moves Qt's focus, ADS follows it, and the current stage
    followed the focus onto another stage's viewport.
  - Panels a switch deletes leave the window at once and are deleted by the event loop; the tests flush
    those deletes at their end, as no event loop runs there.
- **Checked in the app** (2026-11-15, the worktree's Release build, the user's profile restored after):
  two stages, Four View, Look Dev, Side by Side and Default (named Layout then) in turn: each laid out as its file says, the
  button naming it in the accent colour left of the caption buttons, the current stage the same
  throughout. Not checked in the app: the viewports' pictures (the window grab draws them black), the
  menu by mouse, the Open Stage with Options dialog by hand, and a Z-up stage's Four View.
- **Tests**: `tests/workspace_test.cpp` (ui tests: the format's rules, unknown types, the folders and
  groups, switching with two stages, a following Top view, reuse, closing, capture, the controller and
  its menu, following, Open Stage with Options with payloads off) and `tests/orbit_camera_test.cpp`
  (the views on both up axes, orthographic framing).

### 10.2 Changes on 2026-11-16

- The title bar button is a push button on the accent fill with a down arrow, no grid icon (§6.1).
- View > Workspace, and Stages > a stage > Workspace above Views (§6.2).
- *Open Workspaces Folder* is *Open Personal Workspace Folder*, its tooltip the folder.
- `maxWidth` / `maxHeight` on areas and splits, the room left going to the viewports evenly (§3.3). The
  shipped workspaces limit the Outliner column to 320 and the Attributes (or Layers) column to 420.
- Side panels are part of a workspace (§3.2): with an edge and a size, captured by Save Current Layout and
  in how a workspace was left, and **a side panel a workspace doesn't name is hidden**. The shipped ones
  name theirs: the Log collapsed in all but Viewport Only (none) and Scripting (Script Editor on the right,
  Plugins collapsed under it, the Log at the bottom). Viewport Only's `"Plugins"` was the wrong name; the
  panel's objectName is `pluginManagerPanel` (a name is now also matched against the title).
- Choosing the active workspace again lays it out as it is now, not as it was last left (§6.3).

### 10.3 Changes on 2026-11-28

- Switching to a workspace that names panel types that aren't registered shows one warning toast naming
  them (§3.4), from the menu, a shortcut or `usdcc.app.set_workspace()` alike. Before, only an info line in
  the log and the menu entry's tooltip said so. `workspace::apply()` no longer logs them itself: it gives
  them in `ApplyResult::missingTypes`, and the controller says it once for the switch.

## 11. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Duplicate viewports of one stage on switching: close all but one, or keep them as extra tabs? | **Close** (§7.1): "one viewport per stage" stays true, and the area doesn't fill with copies |
| 2 | Coming back to a workspace: its definition again, or how it was left (Maya, Blender)? | **Its definition, every time** (changed 2026-11-16). Phase 4 remembered how each was left, with a *Reset "<name>"*; a panel opened once then came back with the workspace, which no longer matched its file. Choosing a workspace is how the layout is reset; one worth keeping is saved as a workspace |
| 3 | Do panels the workspace places switch to the current stage? | **Yes** (§7.1 step 4): a switch then always gives a predictable result. A panel on another stage is one dropdown away |
| 4 | Do workspaces control side panels? | **Yes**, since 2026-11-16: a workspace has the side panels it names (`sidePanels`, with their edge and size) and hides the others. The first version left unnamed ones alone |
| 5 | Does the Open Stage with Options dialog remember its last choices? | **No**, as asked: the defaults each time. Easy to change later |
| 6 | A four-view's dividers: two rows that split on their own, or one cross that moves all four? | **Two rows** (§3.5.6), as ADS does it. A linked cross would need a splitter of our own inside one dock widget, i.e. one "Four View" panel holding four viewports, which then can't be rearranged by docking |
| 10 | May orthographic views tumble? | **No** (§3.5.3), as Maya by default; a setting could allow it later, turning the view into a perspective one as it starts to turn |
| 11 | Should following viewports reframe on every stage change, or keep a camera per stage? | **Reframe** (by their `fit`), simple and predictable. Keeping one per stage is possible later, kept in the viewport by stage |
| 7 | Floating panels in a workspace? | Later: a `floating` list of entries with a size and position as shares of the screen |
| 8 | ADS's auto-hide sidebars (a panel collapsed to the window's edge)? | Later: an area key `"autoHide": "left"`. Useful for Dependencies or USDA, which are glanced at |
| 9 | Lock a workspace against dragging (Maya)? | Not planned |
| 12 | Groups as headings or as submenus? | **Headings** (§6.2): one click to any workspace, and the list stays short. If a studio ships many, a later `"group": "ACME/Shots"` could make a submenu, without changing version 1 files |
