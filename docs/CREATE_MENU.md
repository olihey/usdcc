# Create Menu

A **Create** menu in the main menu bar, after Edit. It offers every prim type USD knows about, grouped into
categories, each with its Outliner icon. Choosing one adds that prim to the current stage with default values, as one
undo step, and selects it.

## 1. How USD behaves

Run against this build (USD 26.11).

**What's available**
- There are 64 concrete prim types, and asking the schema registry finds them all, so a plugin's types appear by themselves.
- Their plugin isn't a useful category on its own: `usdGeom` holds Cube, but also Camera, Xform and Scope. Three of the 64
  are internal to `execIr` (`IrFkController`, `IrJointScope`, `IrSwitchController`).
- **Three types come in two versions:** Capsule, Cylinder and DomeLight also have `Capsule_1`, `Cylinder_1` and
  `DomeLight_1`. `FindSchemaInfosInFamily()` lists a family, newest first. They differ: `Capsule_1` has `radiusTop` and
  `radiusBottom` instead of `radius`.

**What creating does**
- **"Default values" means nothing is authored:** `DefinePrim` writes `def Cube "Cube" {}`, and the schema's fallbacks do the
  rest (`size` reads 2, `extent` -1 to 1).
- **Some types have nothing to show that way:** a Mesh has no points, and Curves, Points, Volume and Shader are empty too.
  They're valid and appear in the Outliner, but draw nothing.

**A prim without a type**
- `DefinePrim` with no type writes `def "Prim" {}`: valid, defined, and with no schema.
- **It's what a reference wants.** With a reference to a Cube, it composes as a Cube, with `size` reading 2, while its own spec
  stays typeless. A type of its own would win instead: an Xform referencing the same Cube stays an Xform and has no `size`.

**Traps**
1. **An existing name is taken over.** `DefinePrim("/R/Cube", "Sphere")` on an existing Cube silently changes its type, so the
   name must be made unique first.
2. **Under an inactive prim `DefinePrim` fails** with a USD error, so that has to be refused first.
3. **Nothing limits the parent:** USD accepts a Material under a Cube. That's left to the user, as the menu doesn't guess.

**Variant edit mode:** `DefinePrim` under a prim in the variant lands inside it (`/R{m=x}Cube`, nothing at `/R/Cube`). So
creating works in the mode as it stands, and `prim_actions::isEditable()` already says where it can't.

## 2. The menu

```
Create
  In /World                (disabled: where it will go)
  ---------
  Prim (no type)
  Xform
  Scope
  Camera
  ---------
  Geometry    >   Cube  Sphere  Cone  Cylinder  Capsule  Plane
                  ---
                  Mesh  TetMesh  NurbsPatch
                  ---
                  BasisCurves  NurbsCurves  HermiteCurves
                  ---
                  Points  PointInstancer  GeomSubset  GenerativeProcedural
  Lights      >   DistantLight  DomeLight  SphereLight  RectLight  DiskLight  CylinderLight
                  ---
                  PortalLight  GeometryLight  PluginLight
                  ---
                  LightFilter  PluginLightFilter
  Shading     >   Material  NodeGraph  Shader  Backdrop
  Skeleton    >   SkelRoot  Skeleton  SkelAnimation  BlendShape
  Volume      >   Volume  OpenVDBAsset  Field3DAsset  ParticleField  ParticleField3DGaussianSplat
  Render      >   RenderSettings  RenderProduct  RenderVar  RenderPass
  Physics     >   PhysicsScene  PhysicsCollisionGroup
                  ---
                  PhysicsJoint  PhysicsFixedJoint  PhysicsRevoluteJoint  PhysicsPrismaticJoint
                  PhysicsSphericalJoint  PhysicsDistanceJoint
  Other       >   SpatialAudio  LODDistanceHeuristic  LODScreenSizeHeuristic
  Plugins     >   Any type no category lists
```

- **Prim (no type)** comes first, for a prim that will get its type from a reference or payload. Its tooltip says so. Its icon is
  the dashed circle the Outliner already gives a typeless prim, so the two match.
- **Icons:** each entry uses `prim_icons::iconNameForType()`, the Outliner's own lookup, so a plugin's type gets its nearest base
  type's icon. A category's icon is the one of its first entry.
- **Tooltips:** the first paragraph of the type's documentation from its schema, as in the applied schema menu.
- **Built as it opens,** so it's current with the stage, the selection and any plugin loaded since.
- **With no stage open** the whole menu is disabled.
- **Plugins** appears only when it has something in it.

## 3. Creating

`prim_actions::createPrim(undo, stage, parent, type)`, one undo step named "Create Cube" in the edit target.

- **Where it goes:** under the selected prim. With nothing selected or several, at the root of the stage. The first line of the
  menu always says where.
- **During variant edit mode:** with nothing selected, or a prim outside the variant selected, it goes under the prim
  whose variant is being edited, since the root can't be reached from there.
- **Where it can't go:** an instance proxy, an inactive prim, or an unloaded one. Its entries are disabled, and the first line
  says why.
- **Its name** is the type's, made unique among the parent's children: Cube, Cube1, Cube2. An inactive child's name counts as taken.
  A versioned type uses its family's name: Capsule, not Capsule_1. A prim with no type is called Prim.
- **Afterwards** the new prim is selected, so the Attribute Editor shows it, where the name can be changed and the Composition
  section's + adds the reference. The Outliner
  shows it through the stage's change notice, and it needs to scroll to it and expand its parent.

### In the Outliner

The prim menu has a **Create** submenu with the same entries (`addCreateEntries()`).

- **It goes under the clicked prim,** whatever else is selected, so the first line says that prim.
- **Where it can't go,** the submenu is disabled, with the reason as its tooltip. That includes a prim outside the variant being
  edited: a click names the parent, so it doesn't fall back to the variant's prim as the menu bar's does.
- **Nothing to click on, nothing to create:** the Outliner has no menu on its empty space. The menu bar's creates at the root.

## 4. The categories: `PrimCreationCatalog`

A registry, as with `RelationshipTargetRules`: the app fills it with the same calls a plugin uses, so built-in entries get no
special treatment and nothing is hardcoded out of reach.

```cpp
PrimCreationCatalog::Entry entry;
entry.category = "Rigging";
PrimCreationCatalog::instance().set("MyStudioRig", entry);
PrimCreationCatalog::instance().hide("IrFkController");
```

```python
usdcc.usd.set_create_entry("MyStudioRig", category="Rigging")
usdcc.usd.hide_create_entry("IrFkController")
usdcc.usd.remove_create_entry("MyStudioRig")
usdcc.usd.create_entries()   # what the menu lists now
```

- **A category** with no built-in entries appears after the built-in ones, sorted by name.
- **A type with no entry** goes in Plugins, so every available type can always be created.
- **Prim (no type)** isn't a schema type, so it isn't in the catalog: it's always in the menu, and can't be hidden.
- **Types it knows but USD doesn't** are left out of the menu, as their plugin may load later.
- **Not saved:** entries are registered on each run, as with the relationship rules.
- **The internal `execIr` types** are hidden by the built-in entries, and a script can bring them back.

## 5. Decisions

- **Under the selected prim,** with the menu's first line saying where, rather than always at the root: the app can't
  reparent yet, so a prim made in the wrong place would stay there.
- **Still under a selected Gprim** (a Cube, then Create Sphere): one rule is easier to predict, and USD accepts it.
- **Types with two versions make the newest:** Capsule creates `Capsule_1`. usdImaging's Capsule and Cylinder adapters
  handle both versions (`capsuleAdapter.cpp`, `cylinderAdapter.cpp`), and DomeLight_1 has an adapter of its own.
- **Types that start empty stay empty,** as this is about default values. A Mesh with starter content would be authored
  data, and a feature of its own.
- **Curated categories,** rather than grouping by plugin, where Camera would share a list with Cube.
