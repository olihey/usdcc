# Prim Icons — Plan

Status: **phase 1 done** (§7): the Outliner shows the icons. Phases 2 and 3 are still to do.

Give every prim in the Outliner an icon that says what kind of prim it is, for every concrete prim
type OpenUSD ships, with sensible icons for types from plugins the app doesn't know.

## 1. What there was before

- `UsdPrimTreeModel` (Outliner) has one column and no `Qt::DecorationRole`: no icons at all.
- Theme icons are 16px line drawings in light grey (`#a0a0a4`, 1.3–1.4px strokes), generated as
  PNGs at 1x/`@2x` by `tools/gen_theme_icons.py`, because the deployed Qt has no SVG plugins
  (PLAN.md item 24). The UI scale (`UiScaleController`, View > UI Scale) goes beyond 2x, where
  those PNGs blur.

## 2. The prim types

The concrete (instantiable) typed schemas in our OpenUSD (26.11), from the `plugInfo.json`
files: 64, of which 3 are versions of another (`Capsule_1`, `Cylinder_1`, `DomeLight_1`) and 3
are experimental (`execIr`). The outliner also shows prims with no type: a typeless `def`, a
`class` and an `over`.

## 3. Groups: which prims share an icon

One icon per row below. Icon names are [Tabler Icons](https://tabler.io/icons) (§4); *custom*
means drawn for us in the same style. Every light and every primitive shape has its own icon;
icons are shared only by versions of one schema (`Cylinder_1`), by close variants (the curve
types, the Physics joints) and by the abstract fallbacks in §5.

### Structure

| Icon | Prims | Why |
|---|---|---|
| *custom* axes (X/Y/Z lines from a corner) | `Xform` | the transform; the most common prim, so the plainest icon |
| `folder` | `Scope` | a grouping with no transform |
| `circle-dashed` | typeless `def` | a prim with no type |
| `template` | `class` (any type) | something others inherit; drawn instead of the type's icon |
| `square-dashed`, dimmed | `over` with no `def` under it | an opinion with nothing defined |

### Geometry

| Icon | Prims |
|---|---|
| `cube` | `Cube` |
| `sphere` | `Sphere` |
| `cone` | `Cone` |
| `cylinder` | `Cylinder`, `Cylinder_1` |
| `capsule` | `Capsule`, `Capsule_1` |
| `perspective` (a square seen at an angle) | `Plane` |
| `topology-full` | `Mesh` |
| `pyramid` | `TetMesh` |
| `vector` | `NurbsPatch` |
| `vector-spline` | `BasisCurves`, `NurbsCurves`, `HermiteCurves` |
| `grid-dots` | `Points` |
| `grain` | `PointInstancer` |
| `square-toggle` | `GeomSubset` |
| `cube-spark` | `GenerativeProcedural` |

### Camera and lights

| Icon | Prims |
|---|---|
| `video` | `Camera` |
| `arrows-right` | `DistantLight` |
| `sunset` | `DomeLight`, `DomeLight_1` |
| `sun-high` | `SphereLight` |
| *custom* rectangle with rays | `RectLight` |
| *custom* disk with rays | `DiskLight` |
| *custom* tube with rays above and below | `CylinderLight` |
| `window` | `PortalLight` |
| `prism-light` | `GeometryLight` |
| `plug` | `PluginLight` |
| `filter` | `LightFilter` |
| `filter-cog` | `PluginLightFilter` |

The three custom lights share one drawing: the emitter's shape, with short rays on the side it
lights.

### Shading

| Icon | Prims |
|---|---|
| `crystal-ball` | `Material` |
| `topology-star-3` | `NodeGraph` |
| `inner-shadow-top-right` | `Shader` |
| `layout-board` | `Backdrop` (usdUI) |

### Skeletons and animation

| Icon | Prims |
|---|---|
| `walk` | `SkelRoot` |
| `bone` | `Skeleton` |
| `keyframes` | `SkelAnimation` |
| `circle-square` | `BlendShape` |

### Volumes and particles

| Icon | Prims |
|---|---|
| `cloud` | `Volume` |
| `cloud-fog` | `OpenVDBAsset`, `Field3DAsset` (fields) |
| `sparkles` | `ParticleField`, `ParticleField3DGaussianSplat` |

### Render

| Icon | Prims |
|---|---|
| `adjustments` | `RenderSettings` |
| `photo` | `RenderProduct` |
| `stack-2` | `RenderVar` (an AOV) |
| `movie` | `RenderPass` |

### Physics

| Icon | Prims |
|---|---|
| `atom` | `PhysicsScene` |
| `link` | `PhysicsJoint`, `PhysicsFixedJoint`, `PhysicsRevoluteJoint`, `PhysicsPrismaticJoint`, `PhysicsSphericalJoint`, `PhysicsDistanceJoint` |
| `category` | `PhysicsCollisionGroup` |

### Other

| Icon | Prims |
|---|---|
| `volume` | `SpatialAudio` |
| `ruler-2` | `LODDistanceHeuristic`, `LODScreenSizeHeuristic` |
| base-type fallback (§5) | `IrFkController`, `IrSwitchController`, `IrJointScope` (experimental) |

That is 51 icons for 64 types plus `def`/`class`/`over`: 47 from Tabler, 4 custom (Xform and
the rectangle, disk and tube lights). Two more, `bulb` and `cube-3d-sphere`, are only fallbacks
(§5).

## 4. The look

**Library: [Tabler Icons](https://github.com/tabler/tabler-icons) 3.49.0 (MIT), outline set.**
It is the only free set that has all the shapes above: cube, sphere, cone, cylinder, capsule,
bone, keyframes. Lucide (ISC) has no cube, sphere, capsule or mesh; Phosphor (MIT) hasn't
been updated since 2024. Tabler's outline icons are lines with round caps and joins, which
matches the theme's own icons. We copy only the ~44 SVGs we use into the repo, with Tabler's
LICENSE.

**Style:**
- 16px in the Outliner row (scaled with the UI scale), with Tabler's own 2px stroke at 24: about
  1.3px at 16px, the same as the theme's 1.3–1.4px icons.
- **No colour**: every icon is the theme's icon grey `#a0a0a4`, like the other icons in the app;
  the shape alone tells the types apart.
- **Selected rows**: the icon turns white, like the text.
- **States**, without new icons: an inactive (deactivated) prim draws its icon at 35% opacity,
  like its text. A prim with references has a small arrow badge at the icon's lower left, in
  the accent color (the `Appearance/accentColor` setting, blue `#3d8bfd` by default), on selected rows too (`badge-reference.svg`; a circle under it is
  cleared so it reads on any row). Badges for payloads and instance proxies are still to do
  (§7 phase 3).

## 5. Types without their own icon

Prims from plugins (custom schemas) and future OpenUSD types still get an icon: the app walks
up the type's `TfType` bases to the first one with an icon. So a studio's `MyStudioLight`
(derived from `UsdLuxBoundableLightBase`) gets the light bulb, and a custom `Gprim` gets a
generic geometry icon. Fallbacks for the abstract bases:

| Base type | Icon |
|---|---|
| `UsdLuxBoundableLightBase`, `UsdLuxNonboundableLightBase` | `bulb` |
| `UsdLuxLightFilter` | `filter` |
| `UsdGeomCurves` | `vector-spline` |
| `UsdGeomPointBased` | `topology-full` |
| `UsdGeomGprim`, `UsdGeomBoundable` | `cube-3d-sphere` |
| `UsdGeomXformable` | the Xform axes |
| `UsdGeomImageable` | `circle-dashed` |
| `UsdShadeNodeGraph` | `topology-star-3` |
| `UsdPhysicsJoint` | `link` |
| `UsdRenderSettingsBase` | `adjustments` |
| `UsdVolFieldAsset` | `cloud-fog` |
| anything else typed | `circle-dashed` |

Python can add or override icons for a type name (`usdcc.ui.set_prim_icon("MyType", path)`), for
pipelines with their own schemas.

## 6. Decisions (as taken)

1. **SVG at runtime.** `qtsvg` is in vcpkg, and `prim_icons.cpp` renders the SVGs itself with
   `QSvgRenderer` (`Qt6::Svg`, `Qt6Svg.dll` next to the app), so no SVG plugins are needed.
   Icons stay sharp at every UI
   scale and take the theme's grey, or white on a selected row, at runtime (Tabler draws with
   `currentColor`, which we replace before rendering); the theme's own icons and ADS's stock
   SVGs could move to SVG later.
2. **Xform icon**: custom axes (`xform-axes.svg`).
3. **`class` and `over`**: their own icon instead of the type's.
4. **The active checkbox** moved from before the icon to the row's right edge
   (`OutlinerItemDelegate`), so the icons line up down the tree.

## 7. Phases

| Phase | Work | Done when |
|---|---|---|
| **1. Icons in the Outliner** *(done)* | `qtsvg` (decision 1), the SVGs and LICENSE in `src/cpp/usd/resources/prim-icons/`, the `prim_icons` lookup in `usdcc_usd_ui` (type name → icon, the `TfType` fallback, a cache per type, state and scale), `Qt::DecorationRole` in `UsdPrimTreeModel`, white when selected, dim when inactive | Every type in §3 shows its icon; a custom schema shows its base type's; the icons stay sharp at 300% |
| **2. Python** | `usdcc.ui.set_prim_icon()` / `prim_icon()`; the Attributes panel shows the prim's icon beside its path | A pipeline can give its schemas icons from a startup script |
| **3. State badges** | References *(done)*; payloads and instance proxies | — |

Tests: the lookup (each concrete type, the versioned ones, a custom derived type, `def`,
`class`, `over`), and a rendered icon isn't empty at 16px.
