# Edit > Selection

Selects prims of the current stage (docs/CURRENT_STAGE.md), replacing the selection. It's
the same selection the Outliner, the viewports and the other panels show.

## 1. What it covers

Every prim the Outliner lists: inactive, unloaded and abstract ones too, and those inside
instances (instance proxies). Never the pseudo-root. `prim_selection::allPrims()`.

With no stage open, every entry is greyed. The menu is rebuilt each time it opens
(`SelectionMenu::rebuild()`), so the counts and the types and schemas a plugin brings are
current.

## 2. Entries

| Entry | Selects |
|---|---|
| **None** | nothing; greyed while nothing is selected |
| **All** | every prim |
| **Invert** | every prim that isn't selected, and none that is |
| **By Type ▸** | the prims of one type, exactly: Capsule leaves Capsule_1 out |
| **By Schema ▸** | the prims with one applied API schema |
| **By Name...** | the dialog in §3 |

- **By Type** lists every type the Create menu does, grouped the same way
  (`PrimCreationCatalog`, docs/CREATE_MENU.md §4), with the same icons and tooltips, and
  Prim (no type) first. A type the stage has that the Create menu doesn't list (hidden, or
  its plugin isn't loaded) goes in **Hidden or Unknown** at the end.
- **By Schema** lists every applicable API schema by where it comes from, as the Attribute
  Editor's + menu does (`schemaCatalog()`, docs/APPLIED_SCHEMAS.md). A schema counts whether
  it's authored or comes with the prim's type. A multiple-apply schema (CollectionAPI)
  matches any of its instances. Schemas the stage has that USD doesn't know go in
  **Unregistered** at the end.
- Each entry of both shows how many prims have it, right-aligned, and is greyed with none.
  All types and schemas are listed even so.
- No shortcuts: Ctrl+A belongs to the text fields and the USDA view.

## 3. By Name

A dialog (`SelectByNameDialog`) that matches the prims' names, not their paths:

- **Contains the text**: the name has it anywhere. A `.` or `*` is just that character.
- **Regular expression**: the expression matches anywhere in the name (`QRegularExpression`);
  `^` and `$` anchor it.
- **Match case**: off by default, for both.

It says as you type how many prims match, or why the expression isn't valid. **Select** is
greyed while nothing matches. The text and options are remembered for the next time it
opens, in this run of the app.
