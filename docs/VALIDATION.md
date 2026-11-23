# Validation (plan)

Check a stage against OpenUSD's validators and the user's own rules, see what's wrong, go to it, and
fix what can be fixed. Rules can be written in Python without restarting the app.

**Status:** planned 2026-11-23. All five phases built the same day (§13 to §17), but for the MCP
tool: the server isn't built ([MCP.md](MCP.md)). "Checked" means run against this
OpenUSD build (26.08) in a plain Python, unless it says in the app; §11 lists the checks. Everything else is an assumption until
it's tried.

## 1. Decisions

- **OpenUSD's `usdValidation` is the engine,** not one of our own. Its validators, and any a studio
  already has as USD plugins, then work as they are, and our rules are ordinary validators to it.
- **Users don't use its Python API directly.** It can register rules, but §4 lists what's wrong with
  that. `usdcc.validation` wraps it.
- **A fix is an undo step and never saves.** OpenUSD's own fixers save (§7).
- **On demand first.** No re-running on every edit.
- **Rules come from the user's folders and plugins, never from a stage.** Rules are code with the
  user's rights, as plugins are ([PLUGINS.md](PLUGINS.md) §3.6).

## 2. What OpenUSD has

| Piece | What it is |
|---|---|
| **Validator** | A function on a layer, a stage or a prim, returning errors. Its metadata: name (`plugin:Name`), doc, keywords, schema types, time-dependent or not |
| **`UsdValidationRegistry`** | All validators, for the life of the process. A plugin's metadata comes from its `plugInfo.json`, so listing loads no library |
| **`UsdValidationContext`** | A chosen set of validators; runs them in parallel |
| **`UsdValidationError`** | A name, a severity (Error, Warn, Info), a message, data, and *sites*: prim, property, or a spec in a layer |
| **`UsdValidationFixer`** | A repair that belongs to a validator, with `CanApplyFix()` and `ApplyFix()` |
| **Suite** | A named group of validators |

The build has 28 validators: core (3), geometry (4), lights (1), physics (4), shading (9), skeletons
(2), and references and USDZ (5). One has a fixer: `MaterialBindingApiAppliedValidator`. usdcc runs the
physics ones already, in Check Physics ([PHYSICS.md](PHYSICS.md) §5.4).

## 3. Running

### 3.1 The run

`ValidationRun` (Qt-free) takes a stage, a scope (the stage, or prims and what's under them), a set of
rules with their severities and parameters (§6), and a time range. It gives back `Finding`s: rule,
error name, severity, message, sites, data, fixes.

- **What gets visited.** Checked: the default traversal skips inactive prims, `over`s and classes, and
  visits an instance's contents as instance proxies. The panel says what was skipped.
- **Passing prims doesn't recurse.** Checked: `Validate([prim])` visits only that prim, so a selection's
  scope walks the subtrees itself.
- **A rule with two matching schema types runs twice on a prim.** Checked, with `Imageable` and `Mesh`.
  Findings with the same rule, name, message and sites are merged.
- **Findings own plain data** (paths, layer identifiers, strings), not `UsdPrim`s, as they outlive the
  prims. Fixing looks the sites up again.

### 3.2 Threads

Validators read the stage from OpenUSD's worker threads, and the UI thread mustn't edit it meanwhile.
A copy to read on the side, as the USDA panel makes, would hide the edits' layers and composition,
which is what's being checked.

**So:** a run goes in steps on the UI thread, each step itself parallel:

1. stage and layer rules, in one step;
2. prim rules, in batches of prims, sized to take about 50 ms.

Between batches the panel paints its busy card and progress, and Cancel works
([PANEL_BUSY.md](PANEL_BUSY.md)). Editing stays possible between batches; an edit ends the run as
"stage changed", with what it found until then shown as out of date. OpenUSD's validation has no
cancel of its own, so a slow single rule, such as the missing-reference one on a server's files, still
blocks for its time. §10 question 1.

- **The GIL.** The UI thread holds it only while it runs Python, so Python rules can take it on worker
  threads. `ValidationRun` must release it if it was called from Python. OpenUSD's binding does
  (checked: another Python thread kept running during a run).
- **Cost, checked,** on 50,000 bare mesh prims: all built-in validators 0.27 s, after 0.19 s to load
  them; one Python rule, reporting every prim, 0.71 s.

### 3.3 When

- **On demand:** the panel's Run, and Run on Selection in the prim menu.
- **Before saving and before a final render,** both off by default: run the profile and ask on errors,
  with Save Anyway or Render Anyway. Never a block without a way past it.
- **Findings go out of date** when the stage changes after a run. They stay, dimmed, until the next one.

## 4. Why the raw Python API isn't enough

All checked:

| What happens | What the wrapper does |
|---|---|
| One rule raising ends `Validate()`; every other rule's findings are lost | Catches it: one "rule failed" finding, the traceback in the log |
| Registering a name again is an error, and nothing unregisters | Registers one forwarding function per rule, once, and swaps what's behind it. A removed rule forwards to nothing and is hidden |
| Python can't make a `UsdValidationFixer` | Fixes are ours (§7) |
| Rules run on many threads (32 here) | Documented, and a rule can say `main_thread=True` to run in a step of its own |
| `ValidationContext(["studio"])` takes a string as a schema type and fails, unless it's `keywords=` | Not seen by rule authors |
| An error has no identifier until a validator returned it | `report` makes the errors |

A forwarding function keeps the metadata it was registered with. If a reload changes a rule's schema
types, the wrapper registers it under a new internal name (`usdcc:studio.mesh_names#2`) and retires
the old one; the name users see stays.

## 5. Writing rules

### 5.1 Python

```python
import usdcc.validation as v

@v.rule("studio.mesh_names", "Mesh names", types=["UsdGeomMesh"], keywords=["studio"])
def mesh_names(prim, report, prefix="geo_"):
    """Meshes start with the studio's prefix."""
    if not prim.GetName().startswith(prefix):
        report.warn(prim, f"The name should start with {prefix}")

@v.stage_rule("studio.up_axis", "Z up")
def z_up(stage, report): ...

@v.layer_rule("studio.absolute_paths", "No absolute paths")
def absolute_paths(layer, report): ...
```

- **`report.error()`, `.warn()`, `.info()`** take a prim, property, or layer and path, the message,
  and optionally `name=` and `fix=`, one or more of the rule's fixes (`@mesh_names.fix(...)`, §16).
- **Keyword arguments with defaults are parameters.** A profile sets them (§6), and the panel edits
  those that are bool, number, string or list of strings.
- **The docstring** is the rule's doc, shown in the panel.
- **`time_dependent=True`** adds a `times` argument, OpenUSD's `TimeRange`.
- **Rules only read.** They run off the main thread: no edits, no Qt, no `usdcc.app`.

### 5.2 Where rules live

1. **A rules folder:** `%LOCALAPPDATA%/usdcc_config/validation`, then the folders in Settings, then
   `USDCC_VALIDATION_PATH`. Each `.py` is a module. **New Rule...** in the panel writes one from a
   template and opens it in the Script Editor; saving reloads it.
2. **A plugin:** the same decorators, called in `activate()`. They're attributed to the plugin and
   taken back on a reload, as the other calls are.
3. **A USD plugin with C++ validators,** through a usdcc plugin's `"usd"` folder. Registering its
   `plugInfo.json` early should be enough. *Not tried.* These also run in `usdchecker`; whether
   Python rules could, through a Python USD plugin, isn't known and isn't a goal.

A file that fails to import is shown in the panel with its traceback, and the others go on.

### 5.3 Without code

Templates filled in by a form, stored in the profile:

| Template | Asks for |
|---|---|
| Name pattern | Prim types, a regular expression |
| Required attribute or metadata | Prim types, its name, authored or just present |
| Value in a list or range | Prim types, attribute, the values or limits |
| Required API schema | Prim types, the schema |
| Face count | A limit |
| Asset path pattern | A regular expression; relative only; must resolve |
| Kind and hierarchy | Which kinds may be under which |

They run in the engine, without Python (§15). **Copy as Python** turns one into a rule file.

## 6. Profiles

A profile is a JSON file: rules on or off, by name or keyword, with severities and parameters, and the
template instances.

```json
{
  "name": "Film asset",
  "rules": {
    "keyword:UsdzValidators": { "enabled": false },
    "studio.mesh_names": { "severity": "error", "parameters": { "prefix": "msh_" } }
  },
  "templates": [
    { "id": "lowercase", "template": "name_pattern", "types": ["UsdGeomXform"], "pattern": "[a-z0-9_]+" }
  ]
}
```

- **A rule's own entry wins over its group's (`group:studio`), that over a keyword's, and that over
  `*`,** setting by setting. A rule in none is on.
- **Found in** the same folders as rules, and a plugin's `validation/` folder. Those outside the user's
  folder are read-only; Duplicate makes one that isn't.
- **Shipped:** Everything, General (all but USDZ and physics), USDZ, Physics.
- **An unknown rule name is kept** and shown as missing, so a profile survives a plugin that's off.
- **The chosen profile** is an app setting. §10 question 2.

## 7. Fixes

**OpenUSD's `ApplyFix()` saves the edit target's layer** (`fixer.cpp:77`). Checked, on a file: an
unrelated unsaved edit in that layer reached the disk. On an anonymous layer the edit is made and the
save then raises an error. In an editor neither will do.

**So:** the layer's permission to save is taken away while a fixer runs (`SdfLayer::
SetPermissionToSave()`). The edit is made, the save is refused with an error that's taken as the sign
the fix went well, and nothing is written. The plan was to patch OpenUSD; that isn't needed (§16).

Both kinds then run the same way:

- **In `UndoManager::apply()`,** one undo step per click, to the stage's edit target. **Fix All** for a
  rule is one step too.
- **On the UI thread,** with the sites looked up again. A finding whose prim is gone is skipped.
- **Greyed out, with the reason,** when the layer can't be edited, the site is an instance proxy, or
  `CanApplyFix()` says no.
- **A stronger opinion can hide a fix.** The rules are run again on the fixed prims, and a finding that's
  still there says the fix didn't show.
- **A failing fix** is taken back.

## 8. The panel

**View > Validation,** a view panel following its stage.

```text
┌ Validation ────────────────────────────────────────────────────┐
│ [Film asset ▾]  ▶ Run  ⚙    ● 3  ▲ 12  ⓘ 4     [filter      ]  │
├────────────────────────────────────────────────────────────────┤
│ ▾ ▲ Mesh names (12)                                   Fix All  │
│     /World/Props/lamp     The name should start with geo_  Fix │
│ ▾ ● Missing references (3)                                     │
│     /World/Set            tree.usd can't be found              │
├────────────────────────────────────────────────────────────────┤
│ 17 findings in 4,120 prims, 0.3 s · stage changed since        │
└────────────────────────────────────────────────────────────────┘
```

- **Grouped** by rule, prim, or layer; filtered by severity and text. Virtual rows, for 100,000 findings.
- **A click selects the prim;** a property's also shows its row in the Attribute Editor. A layer's site
  opens the USDA panel at the spec, if that can be done.
- **⚙** edits the profile: rules by keyword with their doc, severity and parameters; templates; rule
  files that failed.
- **Right-click:** Ignore Here, Turn Off Rule, Copy, Edit Rule, Show in Outliner.
- **Ignore Here** writes `customData["usdcc:ignoredRules"]` on the prim, as one undo step, so it travels
  with the asset. The panel counts ignored findings and can show them. §10 question 3.
- **Check Physics** stays, as half of it is the simulation's own reports. Its validator half uses
  `ValidationRun`.
- **Later:** a badge on Outliner rows.

## 9. Elsewhere

- **Python:** `usdcc.validation.run(stage=None, prims=None, profile=None)` returns findings;
  `usdcc.validation.rules()`, `.profiles()`.
- **MCP:** `validate_stage`, with severity, rule, and paging, so an agent can check its own work
  ([MCP.md](MCP.md)).
- **Command line:** `usdcc_node validate <file> --validation-profile <name or path> [--rules <folder>]
  [--json] [--fail-on error|warning|info|never]`. It starts without a window and has the plugins and Python already
  ([RENDER_NODE.md](RENDER_NODE.md)). Exit codes: 0 clean, 1 findings, 2 couldn't run.
- **Render farm:** a final render's check (§3.3) runs on the artist's side, before tasks go out.

### 9.1 The code

| Piece | Target | Job |
|---|---|---|
| `validation_run.h` | `usdcc_usd`, Qt-free | `ValidationRun`, `Finding`, batches, merging |
| `validation_profile.h` | `usdcc_usd`, Qt-free | Profiles: read, write, resolve to rules |
| `validation_rules.h` | `usdcc_usd`, Qt-free | Rules known, from OpenUSD's registry and ours; changed signal |
| `validation.py` | `usdcc.validation` | Decorators, forwarding, reload, templates |
| `validation_fix.h` | `usdcc_usd_ui` | Fixes through `UndoManager` |
| `validation_view_panel.h` | `usdcc_usd_ui` | §8 |

`usdcc_usd` links all of `PXR_LIBRARIES`, as Check Physics relies on, so there's no new dependency.
The validators' libraries and `plugInfo.json` files are in the `lib/usd` and `lib` folders `deploy.py`
copies whole.

## 10. Questions

1. **A long stage rule blocks.** Accept it, or run stage and layer rules on a worker under a modal
   card that takes no input? *Proposed:* accept in phase 1, and measure on a real asset.
2. **Should a stage name its profile,** in the root layer's `customLayerData`? It's only a name, so it
   brings no code. *Proposed:* yes, in phase 3, as a suggestion the panel shows.
3. **Where ignores are stored.** In the prim travels, but is an edit to the asset. *Proposed:* the prim.
4. **The save patch.** Our OpenUSD build moves on by rebuilding it, so it's cheap to carry.
   *Not needed:* §16. Worth reporting upstream all the same.

## 11. Checked

With Python 3.13 and the `usdcc_usd` install, 2026-11-23:

- The 28 validators, their keywords and schema types; which has a fixer.
- Python prim, stage and layer rules registering and running. A rule on 50,000 prims ran on 32 threads.
- Everything in §4 and the "checked" remarks in §3 and §7.

**In the app** (phase 1, 50,000 prims, 25,003 findings): all built-in rules in 0.63 s in the panel, the
longest turn of the event loop 0.24 s; an edit ends a run; Check Physics from Python, which holds the
GIL, doesn't deadlock. A raw Python rule's findings show, and a raising one becomes one "rule failed"
finding, with the others' kept. It raising on all 50,000 prims made that run take 41 s, the panel
responsive meanwhile. Phase 2's wrapper stops calling a rule that keeps failing: 1.45 s (§14).

**Not tried:** a C++ validator from a usdcc plugin; Linux; a big production
stage; instances of time-dependent rules.

## 12. Phases

| Phase | What | Done when |
|---|---|---|
| **1. Run and see** | `ValidationRun`, the panel with built-in validators, selecting from findings, busy and cancel, Check Physics moved onto it | Tests: findings of a broken stage by severity and site; a selection's subtree; merging; instance contents; an edit during a run ends it. In the app: run, click, cancel on 50,000 prims |
| **2. Python rules** | `usdcc.validation` (§4, §5.1), rules folder, New Rule, reload on save, plugins | Tests: a raising rule leaves the others' findings; reload swaps code and schema types; a plugin's reload takes its rules back; a run from C++ and from Python doesn't deadlock; `import usdcc.validation` works in a plain Python |
| **3. Profiles and templates** | §6, §5.3, the profile editor, severities, parameters, ignores | Tests: precedence, unknown rules kept, round trip; each template finds and passes |
| **4. Fixes** | §7, the OpenUSD patch, Fix and Fix All | Tests: one undo step, **nothing written to disk**, anonymous layers, a gone prim, a hidden fix reported |
| **5. Elsewhere** | Before save and render, Python `run()`, MCP, `usdcc_node validate` | Tests: exit codes and JSON; Save Anyway; the MCP tool's pages |

Phases 3 and 4 don't depend on each other. Phase 4's own fixes need 2; OpenUSD's need only 1.

## 13. As built: phase 1

`ValidationRun` (`validation_run.h`), the Validation panel (`validation_view_panel.h`), **Validate** in
the Outliner's prim menu, and Check Physics on the same engine. Differences from the plan:

- **Rules are turned on and off by plugin** in the options menu, saved in the app's settings, until
  profiles replace it.
- **A failing rule is a finding already** (`RuleFailed`), from the errors a run posts; which rule it was
  isn't known there.
- **A selection doesn't run rules on the stage and its layers.**
- **An edit to any layer of the stage ends a run,** the session layer and a simulation's cache included.
- **Check Physics runs only the physics rules.** Choosing them by keyword, as it did, also let OpenUSD
  add rules for their types' ancestors.
- **A layer's site shows the layer's text** in the Show USDA dialog, on a double-click, not scrolled to
  the spec. A property's site selects its prim, not its row in the Attribute Editor.
- **The stage and layer rules are one step,** which can't be cancelled (§10 question 1). Not measured on
  a production asset.
- **A step may be longer than 50 ms:** the first batch is a guess, and loading the rules' plugins and
  sorting the findings are each in one turn.

## 14. As built: phase 2

- **`usdcc.validation`** ([validation.py](../src/python/usdcc/validation.py)): `rule`, `stage_rule`,
  `layer_rule`, `Report`, `rules()`, `remove_rule()`, `sync_files()`. It imports in a plain Python.
- **`UserRules`** (`validation_rules.h`, `usdcc_usd`): what it made, for the panel and runs, under the
  ids users gave. OpenUSD has them as `usdcc:<id>#<n>`; those names show nowhere.
- **`ValidationRuleFiles`** (`validation_rule_files.h`, `usdcc_scripting_app`): the folders, watched, so
  a file saved in any editor reloads. Files starting with `_` aren't rules. Each is run as a module of
  its own, so a rules file can't import another: a plugin can. Loaded once the window is up, before
  the startup scripts. `--no-plugins` leaves them out.
- **The panel:** a rule's label and doc, rules on and off by the id's part before its last dot, **Edit
  Rule**, **New Rule...** and **Open Rules Folder**, and a line for each file that couldn't be loaded,
  which opens it. A change to the rules ends a run, or makes the findings out of date.
- **Settings > Validation > Rules folders,** at the next start. `USDCC_VALIDATION_PATH` too.

What differs from the plan, or wasn't decided in it:

- **A failing rule** gives a finding for each of its first 10 failures in a run, where it failed, and
  then isn't called in that run. What it reported before raising counts. The log has the first
  traceback. A run begun by OpenUSD's own context isn't counted as a new one.
- **All of a file's rules or none:** one that fails half way leaves none, nor its earlier version's.
- **Two rules with one id:** the last one loaded wins, with a warning in the log.
- **A retired name is used again** if a rule goes back to the metadata it was registered with.
- **Types** are schema names or classes ("Mesh", "UsdGeomMesh", `UsdGeom.Mesh`), and an unknown one is an
  error when the rule is made, as is an argument without a default.
- **`main_thread` rules** are run by `ValidationRun` itself, so with OpenUSD's own context they run on
  its threads.
- **`data=` isn't there:** OpenUSD's Python error takes none.
- **Parameters** can be read and set from Python only, until profiles.

Checked in the app, on 50,000 prims: a Python rule on 25,000 of them with 18,889 findings, with all
built-in rules, 1.1 s, the longest turn 0.23 s; with another rule failing on every prim 1.45 s. Adding,
saving and breaking a file while running, New Rule, and the Outliner's Validate.

**Not tried:** Linux; rules in `usdcc_node`, which doesn't load them yet (phase 5).

## 15. As built: phase 3

- **`Profile` and `ProfileLibrary`** (`validation_profile.h`, `usdcc_usd`): reading, writing, precedence,
  and resolving to `RunSettings`, what a `ValidationRun` takes. Built in: General (the default),
  Everything, USDZ and Physics. A user's of the same name stands in for one. They are in the rules'
  folders and each plugin's `validation/`, and are read again when a file changes.
- **Checks** (`validation_templates.h`): the seven templates, run natively and in parallel beside
  OpenUSD's validators. Their findings' rule is `check.<id>`.
- **The panel:** the profile's list and its menu (Edit, Duplicate, Delete), Turn Off Rule and Rule
  Settings on a row, Ignore Here and Stop Ignoring, Show Ignored. Phase 1's rules by plugin are gone.
- **The profile editor** (`validation_profile_dialog.h`): rules by group with severity and parameters,
  checks with a form, problems as they're typed. Save stays off while a check that's on won't do.

What differs from the plan, or wasn't decided in it:

- **Checks aren't Python rules.** In the engine they need no registering and work without Python.
  Copy as Python writes the rule out; a test has each find the same places as its check.
- **Parameters belong to a run,** so two panels' runs don't mix. A value that isn't like the default is
  left out, with a warning in the log. A failing rule keeps its own severity.
- **Of several keywords,** off and the most severe win.
- **`usdcc:ignoredRules`** is a list in customData's `usdcc` dictionary, as `:` nests there. Ignoring is
  an undo step, and doesn't make the findings out of date. A finding on an instance proxy, the stage or
  a layer can't be ignored.
- **The stage's wish** (§10 question 2) is read, from `customLayerData["usdcc"]["validationProfile"]`, and
  shown with a link to follow it. Nothing in the UI writes it.
- **The chosen profile** is one for the app. Check Physics doesn't use it.
- **What a profile's file has that isn't known** is kept.

Checked in the app, on 50,000 prims: General 0.44 s; a profile with a Python rule's parameters, a
severity and a check 1.0 s with 58,892 findings, the longest turn 0.18 s; a profile's file added while
running, the menu, the editor, and Ignore Here.

**Not tried:** Linux; a profile from a plugin's folder, in the app.

## 16. As built: phase 4

- **`validation_fix.h`** (`usdcc_usd_ui`): `applyFixes()`, for one finding or many, in one undo step
  over the edit target and the layers findings are in. Then the rules run again on the fixed prims, and
  what's still there is said to be: a stronger layer may hide the fix.
- **A fix that fails** leaves nothing. If some of many fail, the step is taken back and done again
  without them, so a fix may run twice. Not within another open edit, which would go with it.
- **In Python:** `@rule.fix("name", "Label")` on a function that gets what the finding is at and those of
  the rule's parameters it has arguments for, the profile's values; `report.warn(..., fix="name")`.
  A finding in a layer is fixed in that layer. OpenUSD's errors carry nothing else from Python, so the
  fixes' names travel behind the error's in a run of the app's, and are taken off again.
- **In the panel:** a Fix column, with Fix All on a rule's row, and every fix in the menu. The findings
  are brought up to date; undoing makes them out of date.
- **Checks (§5.3) have no fixes.**

**Nothing is written:** a test compares the file before and after OpenUSD's fixer, with another unsaved
edit in the layer, and that a layer whose saving was turned off stays so. In the app, Fix All on 5,000
findings took 0.14 s and its undo 0.01 s, the file as it was.

## 17. As built: phase 5

- **Before saving and before a final render** (Settings > Validation, off): Save Stage, Save Stage As and
  the Render menu and panel run the chosen profile behind a progress dialog, and on errors that aren't
  ignored ask Save Anyway, Show Findings or Cancel. A render from Python, and the Layer editor's own
  saves, aren't asked about.
- **`usdcc.validation.run(stage=None, prims=None, profile=None)`,** in the app and in a plain Python, with
  `profiles()` and `rule_names()`. It takes the `Usd.Stage` itself: putting one into USD's stage cache
  and taking it out again, as the first version did, ends it for the Python that had it.
- **`usdcc_node validate`** (`validate_command.cpp`): `--profile` was taken, so the profile is
  `--validation-profile`, General by default. Rules come from `--rules`, the node's own folder, the
  settings and `USDCC_VALIDATION_PATH`. A rules file that can't be loaded is exit 2: a rule that isn't
  there isn't checked. It needs no OpenGL.
- **The MCP tool isn't there,** as the server isn't. `usdcc.validation.run()` is what it would call, and
  its `run_python` would reach it meanwhile.

**Not tried:** Linux; a render behind the question, from the menu in the app; a production stage.

## 18. Since, and still open

Added after §17: the status line says how long a run took and how many prims it left out (inactive,
only overs, in classes or not loaded, in its tooltip); Group By > Layer; Show in Outliner; a parameter
that isn't a number keeps Save off and says why. A finding whose fix failed, among others that went
well, stays listed.

**Not built:** the MCP tool; jumping to a property's row in the Attribute Editor; scrolling a layer's
text to the spec; Outliner badges; cancelling the stage and layer rules' step; a choice of time range;
writing a stage's suggested profile from the UI.
