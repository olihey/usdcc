# Outliner Name Colours — Plan

Status: **done** (phases 1 and 2, §5), with the recommended choices in §4.

Colour and weight the prim names in the Outliner the way usdview's prim browser does, so a
glance at the tree shows which prims are instances, which have composition arcs, which are only
overs and which are classes. The prim icons (docs/PRIM_ICONS.md) stay as they are.

## 1. What usdview does

From OpenUSD 26.11's `usdviewq` (`common.py` `UIPrimTypeColors`/`UIFonts`,
`primViewItem.py` `_GetForegroundColor()`/`_nameData()`, `utils.cpp` `PrimInfo`).

**Colour**, the first that applies:

| Prim | Colour | RGB |
|---|---|---|
| An instance (`IsInstance()`) | light sky blue | `135, 206, 250` |
| Has composition arcs: authored references, payloads, inherits or specializes, or any variant set | dark yellow | `222, 158, 46` |
| In a prototype, or an instance proxy (`IsInPrototype() \|\| IsInstanceProxy()`) | slate blue | `118, 136, 217` |
| Anything else | light grey | `227, 227, 227` |

An **inactive** prim keeps its colour, darkened: `QColor::darker(150)`, two thirds as bright.

**Font**, by specifier (checked in this order):

| Prim | Font |
|---|---|
| Abstract (a `class`, or under one) | normal |
| Not defined (only `over`s) | italic, light |
| Defined (`def`) | **bold** |

**Tooltip**: "Inactive / Instanced · Defined / Undefined / Abstract · Prototype · *Type* Prim",
then the name, and "Has composition arcs" when it has some. For example "Defined Xform Prim,
Name: Kitchen, Has composition arcs".

usdview's selection (a gold row) and its other columns (visibility, guides, draw mode) are not
part of this.

## 2. What the Outliner did before

- Names are the theme's text colour, `rgba(255, 255, 255, 75%)` (about `191, 191, 191`), and
  white on a selected row (dark.qss).
- An inactive prim is grey (`Qt::gray`) **and italic** (`UsdPrimTreeModel`, `ForegroundRole`
  and `FontRole`). Italic means "over" in usdview, so this clashes.
- The tooltip is the prim's path.
- Instance proxies aren't shown: the tree walks `UsdPrimAllPrimsPredicate`, which stops at an
  instance. So usdview's prototype colour would apply to nothing yet (§4, question 4).

## 3. The change

All in `UsdPrimTreeModel::data()` (and a test); the delegate already draws the model's
`ForegroundRole` and `FontRole`.

| Role | Rule |
|---|---|
| `ForegroundRole` | the table in §1, darkened by 150 when inactive |
| `FontRole` | the font table in §1: bold for `def`, italic for overs, normal for classes. An inactive prim is no longer italic: its darker colour says so |
| `ToolTipRole` | the path, then usdview's line: "Inactive Defined Xform Prim", "Has composition arcs" |

The arcs test is the same as usdview's `PrimInfo`: `HasAuthoredReferences() ||
HasAuthoredPayloads() || HasAuthoredInherits() || HasAuthoredSpecializes() || HasVariantSets()`.
The tree already rebuilds when any of these change (they resync the prim).

Tests (usdcc_ui_tests): a stage with an instance, a referencing prim, a variant set, an over, a
class and an inactive prim; each gets the colour, font and tooltip above.

## 4. Decisions (as taken: the recommended ones)

1. **Plain prims: usdview's `227` grey, or the theme's text colour?** Recommended: **the theme's**
   (`rgba(255, 255, 255, 75%)`), so the Outliner reads like the rest of the app; only the three
   special colours come from usdview. The alternative is usdview's slightly brighter grey.
2. **Bold for every `def`?** usdview makes almost every row bold, since most prims are defined.
   Recommended: **follow usdview** (bold `def`, normal class, italic over), since the weights
   only mean something together. The alternative keeps normal weight for `def` and uses only
   italic for overs.
3. **Selected rows: white, or keep the colour?** Recommended: **keep the colour** on selected
   rows (the selection is only an 18% blue tint, so the colours still read) and white only for
   plain prims; otherwise selecting an instance would hide that it is one. The icon still turns
   white.
4. **Show instance proxies?** usdview lists an instance's children (instance proxies) in the
   prototype colour. The Outliner stops at the instance. Showing them means walking with
   `UsdTraverseInstanceProxies`, and making them read-only (they can't be renamed, deactivated
   or reordered). Recommended: **a separate step after this one**.

## 5. Phases

| Phase | Work |
|---|---|
| **1. Colours, fonts, tooltips** *(done)* | §3: `UsdPrimTreeModel::data()`. On a selected row, `OutlinerItemDelegate` draws a coloured name itself, since dark.qss would turn it white |
| **2. Instance proxies** *(done)* | An instance's children are its instance proxies (`UsdTraverseInstanceProxies`), in the prototype colour, read-only (no rename, no checkbox, no reorder). They are loaded when the instance is first expanded (`canFetchMore()`/`fetchMore()`), so a heavily instanced stage doesn't build every prototype's subtree under every instance; `fetchPath()` loads them on the way to a selected or expanded path |

Tests: `tests/outliner_names_test.cpp`.
