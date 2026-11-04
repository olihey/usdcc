# Applied Schemas

Status: **done** (§7, §8), with the recommended choices in §6.

The Attribute Editor's Applied Schemas row becomes a list of pills, one per schema, each with an × to
remove it. A + at the right opens a menu of the registered schemas, by category, with the ones the
prim already has greyed out. Removing a schema can also clean up the attributes it leaves behind.

Everything marked *checked* below was run against this USD build (26.11), not assumed.

## 1. What there is today

`AttributesViewPanel::buildPrimSection()` shows a read-only text row, "(none)" or a comma-joined
`prim.GetAppliedSchemas()`. Nothing adds or removes a schema.

## 2. Which schemas a prim has: not all removable

`GetAppliedSchemas()` isn't the list to show as it is. *Checked:*

| Prim | `GetAppliedSchemas()` | Authored on the prim (`GetPrimTypeInfo().GetAppliedAPISchemas()`) |
|---|---|---|
| RectLight | `LightAPI`, `CollectionAPI:lightLink`, `CollectionAPI:shadowLink` | none: they come with the type |
| Mesh + MeshLightAPI | `MeshLightAPI`, `LightAPI`, `CollectionAPI:lightLink`, `CollectionAPI:shadowLink` | `MeshLightAPI`: it brings the others |
| Xform + an unregistered `NotARealAPI` | none: **it's left out** | `NotARealAPI` |

Two traps follow:

- **Removing a built-in schema silently does nothing:** `RemoveAppliedSchema("LightAPI")` on a
  RectLight returns true, writes a delete into the layer, and the schema stays. So those get no ×.
- **An unregistered schema is invisible** in `GetAppliedSchemas()`, though it's authored and
  should be removable (a plugin that isn't loaded, or a typo). So the pills come from both lists.

So a pill is one of:

| Pill | Looks | × | Tooltip |
|---|---|---|---|
| **Authored** on this prim | normal | yes | The schema's documentation |
| **Built in:** from the prim's type, or another schema | dimmed | none | "Comes with RectLight" / "Comes with MeshLightAPI" |
| **Not registered:** authored, but USD doesn't know it | amber | yes | "Not registered: its plugin may not be loaded" |

An instance proxy is read-only: no × and no +.

## 3. The row

```
 Applied Schemas  ( LightAPI ) ( MaterialBindingAPI × ) ( CollectionAPI:ferns × )            [+]
                  ( PhysicsRigidBodyAPI × )
```

- Pills wrap to more lines when they don't fit (a small `FlowLayout` in usdcc_ui; Qt has none), with
  the + staying at the row's right edge.
- A pill's tooltip also lists the properties the schema defines.

## 4. Adding: the + menu

### 4.1 Categories: yes, from USD itself

Each schema comes from a plugin, and `PlugRegistry::GetPluginForType()` says which, so the
categories need no hand-kept list of schemas. *Checked:* this build has 59 API schemas, of which 5
are non-applied (`ClipsAPI`, `ModelAPI`, `PrimvarsAPI`, `XformCommonAPI`, `ConnectableAPI`) and are
left out, as they can't be applied. The other 54:

| Category | Plugins | Schemas |
|---|---|---|
| Collections & Color | `usd` | CollectionAPI ⁿ, ColorSpaceAPI, ColorSpaceDefinitionAPI ⁿ |
| Geometry | `usdGeom` | BackPlateAPI ⁿ, GeomModelAPI, MotionAPI, VisibilityAPI |
| Hydra | `usdHydra` | HydraGenerativeProceduralAPI, HydraPrimAPI, HydraRenderPassAPI |
| Level of Detail | `usdLod` | LODOverrideAPI, LODRootAPI |
| Lighting | `usdLux` | LightAPI, LightListAPI, ListAPI, MeshLightAPI, ShadowAPI, ShapingAPI, VolumeLightAPI |
| Media | `usdMedia` | AssetPreviewsAPI, AuthorshipAPI ⁿ |
| Physics | `usdPhysics` | PhysicsArticulationRootAPI, PhysicsCollisionAPI, PhysicsDriveAPI ⁿ, PhysicsFilteredPairsAPI, PhysicsLimitAPI ⁿ, PhysicsMassAPI, PhysicsMaterialAPI, PhysicsMeshCollisionAPI, PhysicsRigidBodyAPI |
| Profiles | `UsdProfiles` | ClaimsAPI |
| Semantics | `usdSemantics` | SemanticsLabelsAPI ⁿ |
| Shading | `usdShade`, `usdMtlx`, `usdRi` | CoordSysAPI ⁿ, MaterialBindingAPI, NodeDefAPI, MaterialXConfigAPI, RiMaterialAPI, RiSplineAPI, StatementsAPI |
| Skeleton | `usdSkel` | SkelBindingAPI |
| UI | `usdUI` | AccessibilityAPI ⁿ, NodeGraphNodeAPI, SceneGraphPrimAPI |
| Volumes & Particles | `usdVol` | The 11 ParticleField...API schemas |

ⁿ multiple-apply.

- **The only hand-kept part** is plugin name to friendly name, a small table. A plugin it doesn't
  know, such as a studio's or a renderer's, gets its own category under its plugin name, sorted in,
  so its schemas appear with no code change.
- **Order:** alphabetical, as above. Submenus keep the top level to 13 entries.

### 4.2 The menu

```
 [+] ▸  Collections & Color  ▸
        Geometry             ▸   GeomModelAPI
        Lighting             ▸   MotionAPI
        Physics              ▸   VisibilityAPI       (greyed: already applied)
        ...                      BackPlateAPI...     (multiple-apply: asks for a name)
```

An entry is greyed out, with its reason as the tooltip, when:

- **Already on the prim:** applied or built in. Multiple-apply schemas never are: a prim can have
  many, so they end in "..." and ask for a name.
- **It can't apply here:** `UsdPrim::CanApplyAPI()` says why, in USD's own words. *Checked:* on an
  Xform, MaterialXConfigAPI gives "can only be applied to prims that are of following type, or have
  following API schema applied: Material."

An entry's tooltip is otherwise the schema's documentation, from its prim definition.

### 4.3 Multiple-apply schemas

A small dialog asks for the instance name, refusing an invalid or already used one
(`IsAllowedAPISchemaInstanceName()`). *Checked:* `ApplyAPI(type, "foo")` gives `CollectionAPI:foo`,
and two instances coexist.

### 4.4 Adding its attributes

After a schema is picked (and named, if it's multiple-apply), a question lists the attributes it would
add: **Apply Schema Only**, **Apply and Add Attributes** or **Cancel**.

- **Add** authors each one on the prim, in the edit target and the same undo step, with the schema's
  fallback value, so it's in the layer and the USDA text. One with no fallback is declared without a
  value.
- **Listed:** the attributes the schema defines, its nested schemas' included, that aren't authored on
  the prim yet (`schemaAttributesToAdd()`). So nothing existing is overwritten.
- **Not relationships:** an empty relationship says nothing.
- **No question** when there's nothing to add, such as for `MaterialBindingAPI`.

## 5. Removing: the × and the cleanup question

The × removes the schema from the edit target, one undo step. *Checked:* a schema applied in a
weaker layer is removed by a delete in the edit target's list op, and that works.

**What a removal leaves behind**, before asking, without changing anything:

1. Build the prim's definition without the schema: `UsdSchemaRegistry::BuildComposedPrimDefinition()`.
2. The properties in today's definition but not that one are **orphaned**. This handles a schema's
   nested schemas and instances, and keeps a property another schema still defines. *Checked:*
   with RigidBody and Mass applied, removing RigidBody orphans `physics:velocity`, and not
   `physics:mass`. A custom attribute is never in the list.
3. Of those, **authored ones** are what the question is about. Properties with only fallback values
   aren't on the prim anyway.

**The question**, when anything authored is orphaned:

```
 Remove PhysicsRigidBodyAPI from /World/Box?

 It leaves 2 authored attributes behind:
     physics:velocity
     physics:kinematicEnabled
 1 more is in anim.usda, so it can't be removed from here.

              [Remove Schema Only]  [Remove and Clean Up]  [Cancel]
```

- **Clean Up** removes those property specs from the edit target, in the same undo step.
- **Other layers:** *checked,* a removal leaves their values in place, and they stay on the prim as
  plain attributes. The question lists how many, as they can't be removed from here without editing
  other layers.
- **A known gap:** *checked,* `MaterialBindingAPI` defines no properties, because `material:binding`
  relationships are per-purpose. So nothing would count as orphaned for it (§6, question 3).

## 6. Decisions to make

1. **Ask when nothing authored would be left behind?** Recommended: **no**. It removes at once, since
   there'd be nothing to choose between; undo is there either way. The alternative is asking every
   time.
2. **Categories?** Recommended: **by plugin, with friendly names** (§4.1). The alternative, a
   hand-kept list by purpose, reads slightly better but goes stale and misses studio schemas.
3. **`MaterialBindingAPI`'s bindings?** Recommended: **count `material:binding` and
   `material:binding:*` as its properties**, a one-line exception, so Clean Up does what it says. The
   question lists them by name first, and removing them changes how the prim looks. The alternative
   is leaving them out and saying nothing.
4. **Hide or grey what can't apply here?** Recommended: **grey with the reason**, as you asked for
   ones already applied. It also teaches what each needs. Hiding shortens the menus.
5. **A filter in the menu?** Recommended: **not now**. With 54 schemas in 13 submenus, finding one is
   quick, and a filter box inside a menu is awkward in Qt.

## 7. Phases

| Phase | Work | Tests |
|---|---|---|
| **1. The logic** *(done)* | `applied_schemas.h` (Qt-free, usdcc_usd): `appliedSchemas(prim)` with each one's state and what brings it; `schemaCatalog(prim)` by category, with why each is unavailable; `orphanedProperties(prim, schema)` split by layer. `prim_actions::applySchema()` and `removeSchema()` (with or without cleanup), each one undo step | A type's schema, a nested one, an unregistered one; categories, including an unknown plugin's; not applied ones left out; orphans with a shared property, nested schemas, instances, another layer; each edit and its undo |
| **2. The pills** *(done)* | `FlowLayout` (usdcc_ui); the pills in the Prim section, styled in `dark.qss`; × and the question | Pills per state, × only where it works; wrapping; the question appears only when needed, and each answer; read-only on an instance proxy |
| **3. The menu** *(done)* | The + menu by category, greyed entries with reasons, the instance name dialog | Greyed for applied and can't-apply; multiple-apply never greyed; an invalid or used name refused; applying adds its pill and its attributes' sections |

## 8. As built

- **`applied_schemas.h`** (usdcc_usd, Qt-free): `appliedSchemas()`, `schemaCatalog()`,
  `canApplyInstance()`, `orphanedProperties()`.
- **`prim_actions::applySchema()` and `removeSchema()`**, each one undo step. `removeSchema()` refuses a
  built-in schema.
- **`SchemaPillsEditor`** (`properties/schema_pills.h`) and **`usdcc::ui::FlowLayout`**. The + is the
  editor's `rowButton()`, so it's the row's rightmost control.
- **`AttributesViewPanel`:** `populateSchemaMenu()`, `applySchemaInteractively()`,
  `removeSchemaInteractively()`. Applied schemas are part of the layout signature, so a change rebuilds
  the panel, and new attributes get their rows.
- **Narrow panels:** a long schema name doesn't widen the row. Its pill is given the row's width and the
  name is cut short, with the × still showing.
- **If a schema is both authored and from the prim's type,** it counts as built in, since removing it
  would change nothing.
- **Limits:**
  - A property with a spec in the edit target and in another layer is removed only from the edit
    target, so the weaker value then shows. The question counts it among those it can clean up.
  - Only the instance name dialog's result is unchecked by tests: the name rules are tested through
    `canApplyInstance()`, not by typing into the dialog.
