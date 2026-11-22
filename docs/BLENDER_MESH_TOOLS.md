# Blender mesh tools

*Updated 2026-11-15. The six editing commands, jobs, conversion, and distribution
tooling are implemented. Windows x64 and Ubuntu 24.04/WSL builds and tests pass.
Windows and Linux distributions have artifact-specific license/source reviews and
integrity gates, including exact Ubuntu source packages for the Linux configuration.
See section 15 and [the review](BLENDER_DISTRIBUTION_REVIEW.md). The plugin is built and enabled in every app build. Tracked as M14 in
[PLAN.md](PLAN.md).*

usdcc will offer mesh operations through an optional native C++ plugin that uses
Blender's BMesh algorithms. OpenUSD remains the authoritative scene representation:
the plugin converts selected geometry to a temporary BMesh, evaluates an operation,
and authors the validated result into the original USD stage as one undoable edit.

Our independently written adapter and plugin code will use BSD-3-Clause. Blender
code retains its upstream licenses. Distribution of the linked configuration will
follow the GPL compliance policy in section 9.

## 1. Decisions

| Area | Decision |
|---|---|
| Integration | Native plugin loaded through the existing [C++ SDK](PLUGIN_SDK.md), with private Blender implementation libraries |
| Plugin | ID `org.usdcc.blender-mesh-tools`, user-facing name `Mesh Tools` |
| Scene ownership | The existing `UsdStage`, its layer stack, and usdcc's undo manager own all persistent edits |
| Working geometry | Temporary BMesh for each operation; no parallel persistent Blender scene |
| Source baseline | Blender 5.2.4 LTS, pinned to `cd5dc428cce1993a4b35986f227c6a24e42a365c` with archive SHA-256 in `deps/blender.json` |
| Build | `USDCC_BUILD_BLENDER_MESH_TOOLS`, on by default ([BUILD_OPTIONS.md](BUILD_OPTIONS.md)); `tools/build.py` builds the pinned backend before app configuration, unless `--without blender_mesh_tools` |
| Platforms | Windows x64 first, then Linux x64 before the first supported release; macOS deferred |
| Execution | One persistent serialized backend executor; owned numeric buffers cross the job boundary |
| Packaging | Separate standard artifact without Blender code and optional artifact containing the GPL-compliant linked configuration |

The native backend uses BMesh construction and algorithm APIs directly. Python,
`bpy`, `.blend` files, Blender UI, and Blender USD import/export are outside this
integration. The app's Python and OpenUSD builds remain its existing dependencies.
The backend must not introduce a second OpenUSD or Python runtime into the process.

## 2. First release

The first release supplies whole-mesh commands for selected `UsdGeomMesh` prims:

| Command | Parameters and behavior |
|---|---|
| Triangulate | Selected meshes; configurable triangulation method, preserving corner data and material membership |
| Merge by Distance | Distance in mesh-local units; preserve protected UV, material, and sharp boundaries |
| Limited Dissolve | Angle limit; preserve protected UV, material, and sharp boundaries |
| Recalculate Normals | Make winding consistent where possible and regenerate the declared output normals; report ambiguous components |
| Decimate | Target ratio; expose boundary/seam preservation; report the achieved count when constraints prevent the requested reduction |
| Bevel | Width, segments, profile, angle limit, overlap clamp, and new-face material policy; only eligible manifold edges |

An operation's boundary and attribute guarantees must be demonstrated for the pinned
backend before its command is enabled. Reject a case the implementation cannot
preserve; do not silently relax constraints to make an algorithm succeed.

One invocation may process several meshes in the same stage. Validate the entire
selection before starting, then commit all results as one undo step or none.
Deduplicate selected paths. Selection remains at the prim level in this release.

Component selection, interactive extrusion/inset/bridge, booleans, UV unwrapping,
remeshing, general modifier stacks, Geometry Nodes, rendering, and simulation are
later work. A BMesh integration alone does not provide all of Blender's systems.
Continuous previews and persistent procedural modifier recipes are also deferred.

## 3. Native dependency and module boundary

BMesh is an internal Blender library. Its upstream build has dependencies on
Blender kernel, utility, DNA, allocation, and other support code. Phase 1 must
establish the actual source, link, initialization, and shutdown requirements for
our operations; extracting a small standalone library is not assumed to be trivial.

Planned implementation layout:

```text
deps/blender.json                         immutable source and build configuration
tools/build_blender_mesh.py               explicit dependency build
src/plugins/blender_mesh_tools/
  CMakeLists.txt
  src/                                   usdcc menus, jobs, USD snapshot and commit
  backend/                               private BMesh conversion and operations
  licenses/                              upstream notices and dependency inventory
tests/blender_mesh_*                      optional backend and app integration tests
```

Keep the Blender build separate from usdcc's top-level CMake configuration. Start
with upstream source targets and determine the required link closure. If a smaller
source subset is necessary, maintain its source list and minimal patches explicitly.
Do not distribute an unexplained extraction or rely on linker elimination to remove
license obligations. No Blender source is copied into BSD-owned files.

Match the plugin SDK's compiler, runtime, and build requirements, including the
release CRT on Windows. Hide Blender symbols inside the implementation and export
only the usdcc plugin entry points. Keep allocation and freeing of BMesh objects in
the backend. No Blender types appear in public usdcc headers or Python bindings.

Record the source commit, patches, compile options, dependency versions, and license
inventory beside each built backend. Retain the existing SDK `built_for` checks;
include the backend revision in diagnostic/build metadata. Dependency upgrades are
explicit changes followed by conversion and operation regression checks.

## 4. USD input contract

Capture composed geometry from the live stage, including unsaved layer and session
opinions. Resolve data on the UI thread into an owned snapshot before background
work begins. Preserve the original path, schema, transforms, relationships, and
material prims; only author the operation's required geometry and subset changes.

The first release accepts static polygon meshes with `subdivisionScheme = none`,
valid finite coordinates, valid polygon indices, and supported attributes. It uses
default-time geometry. Reject time-varying geometry or domain-dependent attributes,
value-clip geometry, skinning/blend-shape bindings, authored holes, and subdivision
creases/corners until their behavior has a dedicated implementation. Do not convert
a subdivision cage into polygons implicitly.

Meshes composed through references or payloads may be edited by stronger opinions
in a writable local layer. Reject instance proxies and instanceable mesh editing;
do not implicitly deinstance. First-release edit targets are writable local layer
stack targets without variant or reference-path mapping. Variant-context editing
and mapped external edit targets are later work. Revalidate the authoring target
before commit, including whether a stronger opinion would hide the result.

Operate in mesh-local space. Preserve the prim's orientation token and transforms,
including non-uniform scale. Define conversion winding consistently with the
orientation token and restore it on output. Label distances as mesh-local units;
do not promise world-space widths. Preserve stage axis and unit metadata.

## 5. Geometry and attribute conversion

| USD data | Working representation and output rule |
|---|---|
| `points` | BMesh vertices; retain or regenerate a source-point map |
| Polygon counts and indices | BMesh faces and ordered loops; regenerate both arrays together |
| Face-varying UVs and colors | Per-loop custom data; preserve seams and regenerate value/index arrays consistently |
| Supported vertex attributes | Per-vertex custom data with explicit interpolation rules for new vertices |
| Supported uniform attributes | Per-face data with explicit inheritance rules for split or new faces |
| Constant attributes | Preserve unchanged on the original prim |
| Material subsets | Carry source-face material IDs; rebuild affected subset membership with existing material paths |
| Normals | Preserve when valid or regenerate using the command's declared policy; write matching interpolation metadata |
| `extent` | Recompute from the resulting local-space points |

The conversion layer owns a registry of supported types, domains, and interpolation
policies. Support UV `texCoord2f`/`float2`, float color attributes, and required
material/source IDs first. Preserve indexed and unindexed forms semantically even
when the output requires a different index buffer. Indexed attributes inherited
from an ancestor must be resolved and locally overridden when the operation changes
their domain. Unsupported topology-dependent attributes make that operation
inapplicable; arbitrary USD primvars are not assumed to survive BMesh automatically.

Integer IDs and categorical values must not be averaged. When an operation creates
faces, assign materials by declared provenance rules. Bevel defaults to an adjacent
source-face material, with a deterministic tie rule and an explicit existing-material
override. Reject unsupported or overlapping subset families and ambiguous material
membership instead of discarding subsets. Retain subset relationships and metadata.

Maintain source-to-result maps for points, faces, and corners where possible. Verify
each operation's custom-data behavior; source indices are not guaranteed stable
after welding, dissolving, beveling, or decimation. Changes to winding must also
reorder corner attributes. Validate the final arrays before any live authoring.

## 6. Jobs and user interaction

Add `Mesh/...` menu actions and an Outliner `Mesh` submenu using the existing SDK.
An options dialog supplies operation parameters and explains input incompatibilities.
Keep backend details in diagnostics rather than requiring users to configure Blender.
Use existing plugin settings, busy/progress UI, notifications, and logging.

Each job captures stage identity, selected prim paths, edit target, relevant stage
generation, effective geometry/attribute inputs, and operation parameters. Use an
owned cancellation token and plain result buffers across the execution boundary.
The background executor never reads or writes the live stage or Qt widgets.

Begin with one serialized executor for all backend calls, including initialization
and teardown. Phase 1 must verify that the selected source subset can run there;
if it requires application-main-thread work, resolve that integration before enabling
the UI. Do not assume either that all BMesh code is reentrant or that `bpy` threading
restrictions describe the native subset.

Before committing, reject a result if its stage closed, its prim changed identity,
its relevant inputs or composition changed, or its edit target changed. Switching
the current stage alone does not redirect the result. A conservative per-stage
generation counter may reject additional stale jobs in the first implementation.

Cancel immediately discards a result. Interrupt the algorithm only at demonstrated
safe checkpoints; a non-interruptible native call finishes before resources are
released. Never terminate its thread. At shutdown, cancel queued work and join the
executor before destroying the backend or application objects it requires.

The plugin runs in-process: a native crash can terminate usdcc. Validate input,
apply memory/job limits, and test malformed meshes. This design does not promise
the crash isolation of a helper process.

## 7. Commit and undo

Build and validate the complete result in scratch layers or owned buffers first.
On the UI thread, recheck all preconditions and open a fresh undo edit session for
the captured stage/target. The automatic menu callback scope has already ended
when asynchronous work completes.

Author complete, mutually consistent property arrays. Author compatible local
primvar values, indices, interpolation metadata, normals, extent, and subset indices
where required; remove or block stale weaker opinions when appropriate and supported.
Keep referenced source assets unchanged. Batch notices with `SdfChangeBlock`, but
do not treat that block as a transaction or query composition inside it.

Use `beginEdit`/`endEdit` and `cancelEdit` for rollback through the existing
[UndoManager](../src/cpp/usd/include/usdcc/usd/undo_manager.h). Check every authoring
result. On an error, restore all edits made by the invocation before reporting it.
If the existing recorder does not cover an edit needed for subset creation or
cleanup, extend and verify it before shipping the command.

One successful invocation creates one named step on the originating stage. No-op,
cancelled, stale, and failed invocations create none. Redo restores the stored USD
changes without re-running Blender. The existing stage notices refresh Hydra and
the panels. Verify the composed result after authoring and roll back a commit that
does not produce the intended geometry.

## 8. Implementation phases

| Phase | Deliverables | Exit criterion |
|---|---|---|
| 1. Build and licensing feasibility | Pin source; inventory dependencies; standalone native harness; allocator/init/shutdown; plugin activation/shutdown checks; deployment review | Construct a BMesh, triangulate and bevel test geometry, free it, and repeat in a standalone harness and the loaded plugin without a Blender scene, Python, or second USD runtime |
| 2. USD conversion | Snapshot/result types; topology and supported attribute conversion; applicability checks; source maps; validation | Unmodified round trips preserve polygon winding, UV seams, materials, transforms, and unrelated USD opinions; unsupported inputs leave the stage untouched |
| 3. Jobs and transactions | Serialized executor; cancellation; stale-result checks; settings/dialog framework; fresh undo commit scope and rollback | Triangulate completes through the actual menu action with one undo step; cancellation, edits during work, stage closure, and commit failure are handled correctly |
| 4. First-release operations | Merge, dissolve, normals, decimation, bevel; per-operation data policies and diagnostics | Each enabled command meets its topology, boundary, attribute, and undo contract on fixtures and through the app |
| 5. Supported distribution | Windows and Linux builds; dependency/source bundles; notices; optional CI and deploy; clean-machine verification; dependency compatibility review | Standard artifact remains free of Blender code; optional configuration is reproducible, license-complete, and passes the supported platform checks |

Phase 1 is a feasibility gate, not proof that Blender exposes a supported standalone
SDK. If the source closure or lifecycle requires substantially more Blender runtime,
record the cost and revise the native integration before proceeding. Do not silently
substitute `bpy` or a helper process. The Windows phase 1 results are recorded in
section 12; section 13 records USD conversion. Sections 14–15 record the editing
and distribution implementation. The supported-release gate still requires final
license/source review; implementation and local package tests do not waive it.

## 9. Licensing and distribution

Retain BSD-3-Clause for usdcc and our independently written plugin/adapter source.
Keep upstream notices on all Blender source and modifications. Inspected BMesh files
carry GPL-2.0-or-later; select GPL-3.0-or-later for the linked distribution
only after checking the complete pinned dependency set permits it.

Treat the in-process app/plugin configuration as a combined work for compliance.
An optional DLL, separate download, or BSD adapter is not a copyleft boundary.
Do not change the repository's root license or describe the linked artifact as
BSD-only. Existing BSD components retain their license and notices.

Before publishing the optional configuration, review the complete linked dependency
set, including OpenUSD/TOST, Qt/PySide/ADS, and optional SDK integrations. Exclude
proprietary render plugins/SDKs from that artifact unless their terms have been
confirmed compatible. The ordinary BSD distribution remains separately maintained.

Provide GPL texts, copyright notices, third-party notices, exact source archives,
our patches, and the scripts/configuration needed to build and install the covered
combination. Include corresponding usdcc and adapter source; publish clear source
access instructions beside the binaries. Preserve the existing LGPL replacement
requirements. Have the actual distribution and license inventory reviewed before
release; this is a release requirement, not a requirement to publish private work.

The first supported package includes the native backend and does not require an
installed Blender application. Record the backend revision and licenses in the
plugin diagnostics and About/license inventory. Use Blender's name descriptively
for attribution, with usdcc branding; do not imply Foundation endorsement.

## 10. Acceptance and performance

- Test polygon meshes with n-gons, boundaries, multiple connected components, both
  winding conventions, UV seams, indexed colors, material subsets, and composed
  referenced assets with unsaved overrides.
- Test non-manifold, degenerate, duplicate, out-of-range, NaN/Inf, empty, and oversized
  inputs. Every operation either produces valid output within its contract or fails
  with no authored changes. Bevel's manifold-edge preconditions are explicit.
- Compare selected algorithm results against the same pinned Blender implementation
  on controlled fixtures, using topology/attribute invariants and numerical tolerance
  rather than element ordering or screenshots alone.
- Exercise actual menu dispatch, stage switching/closure, edit-target changes,
  edits during jobs, cancellation, shutdown, no-ops, failed authoring, undo, and redo.
- Verify that referenced source layers, material networks, transforms, references, variants,
  and unrelated custom metadata remain unchanged when writing a local geometry override.
- Add optional CTest targets for the adapter, operations, and app integration. Run
  memory/lifetime checks on the backend and a plugin-free default build regression.
- Measure snapshot, conversion, algorithm, commit, and total time, plus peak memory,
  on approximately 10k, 100k, and 1M-face fixtures where practical. Report hardware,
  build configuration, actual input/output counts, and cancellation latency. Record
  baseline budgets before phase 5; do not claim interactive performance without them.

## 11. Upstream references

- [Blender releases](https://www.blender.org/releases/) for the LTS baseline; freeze
  the exact tag/commit and archive hash in the dependency manifest.
- [BMesh build definition](https://github.com/blender/blender/blob/main/source/blender/bmesh/CMakeLists.txt)
  and [mesh construction API](https://github.com/blender/blender/blob/main/source/blender/bmesh/intern/bmesh_core.hh).
- [Bevel implementation](https://github.com/blender/blender/blob/main/source/blender/modifiers/intern/MOD_bevel.cc)
  and [decimation API](https://github.com/blender/blender/blob/main/source/blender/bmesh/tools/bmesh_decimate.hh).
- [Blender licensing](https://www.blender.org/about/license/),
  [GNU guidance on linking](https://www.gnu.org/licenses/gpl-faq.html#LinkingWithGPL),
  [GNU guidance on plugins](https://www.gnu.org/licenses/gpl-faq.html#GPLPlugins),
  and [GPLv3 distribution requirements](https://www.gnu.org/licenses/gpl-3.0.html).

The `main` source links explain the API and dependency structure; implementation
must use and audit the pinned release revision.

## 12. Phase 1 implementation and verification

At the phase 1 checkpoint, the native feasibility gate passed on Windows x64 with MSVC 19.44.35229.0, using
the release CRT. The standalone harness and the plugin call the same private DLL.
The initial plugin registered **Debug > Blender Mesh Backend Probe**. Section 14
adds the live editing commands; the diagnostic remains available.

The build uses an explicit upstream translation-unit list and eight generated GPL
source subsets. The subsets retain BMesh construction, triangulation, bevel, normal,
custom-data, and required supporting routines, while excluding application/object
management and serialization entry points. External custom-data IO callbacks are
disabled. Missing application APIs are not implemented as stubs. The generation
script and source hashes are included in the development inventory. This extraction
is our maintained integration, not an upstream standalone SDK.

Initialization, mesh operations, freeing, and teardown run on one worker at a time.
The harness repeats quad triangulation and whole-cube bevel over 1, 100, and 10
iterations across fresh worker/runtime cycles. It checks the resulting counts,
manifold cube edges, finite bounded coordinates, and zero net guarded allocations
per cycle. Quad triangulation produces 4 vertices, 5 edges, and 2 faces. A cube
beveled with one segment produces 24 vertices, 48 edges, and 26 faces.

At the phase 1 checkpoint the Windows backend DLL was 396,288 bytes in the measured
RelWithDebInfo build. `dumpbin` showed only the two diagnostic C exports and imports from PThreads4W plus
the Windows C/C++ runtime; it imports no Python, Qt, OpenUSD, or TBB runtime.
This is a footprint observation for the current probe, not a size or performance
guarantee for subsequent operations.

Build and exercise it from the repository root with Python 3.12 or later:

```powershell
python tools/build_blender_mesh.py --fetch
cmake -S . -B build -DUSDCC_BUILD_BLENDER_MESH_TOOLS=ON
cmake --build build --config RelWithDebInfo --target usdcc_python_tests --parallel 4
ctest --test-dir build -C RelWithDebInfo -R usdcc_blender_mesh --output-on-failure
```

The first command explicitly fetches and verifies Blender, installs the private
vcpkg dependencies, builds the native DLL/harness, runs its CTest check, and installs
development files under `external/blender-mesh-install`. Later builds need no
Blender download. `--work` and `--install` select other locations; a nondefault
install is supplied to usdcc as `USDCC_BLENDER_MESH_ROOT`.

The plugin is built under `build/plugins/blender_mesh_tools` and copied into the
built app's shipped-plugin directory. Every app build includes it; an old OFF cache
value is replaced with ON. Direct CMake configuration requires a previously built
backend; the build helper handles that step. Ordinary deploy excludes
both its source and stale compiled output from the examples package on Windows and
Linux. `--with-blender-mesh-tools` produces the separate combined candidate in section 15.

Verified checks:

- Standalone native harness passes with repeated allocator and worker lifecycles.
- Two real plugin-manager activation/menu/shutdown cycles pass, without authoring
  USD or creating undo entries.
- Five source-fetch checks cover explicit network opt-in, checksum mismatch,
  unexpected archive roots, traversal rejection, and changed source pins.
- The existing `usdcc_python_tests` scripting/plugin regression suite passes with
  the optional plugin tests included.
- Before the always-enabled build policy, the regular application was also
  verified without Blender. Current builds always include the plugin.
- New Python tooling passes Ruff and compilation checks.

The install prefix records compiler/platform information, source and excerpt hashes,
Blender notices, and vcpkg SPDX/notice records for Eigen 3.4.0, fmt 12.2.0, and
PThreads4W 3.0.0. `EIGEN_MPL2_ONLY` is enabled; Blender's TBB/Python/internationalization
features are disabled in this subset. These are development inventory files, not
a completed corresponding-source release bundle. Section 13 extends this backend
with conversion and input checks. Linux verification is recorded below; the final
Windows combined-distribution review is recorded in `BLENDER_DISTRIBUTION_REVIEW.md`;
Linux has its own source receipt and review in `BLENDER_LINUX_DISTRIBUTION_REVIEW.md`.

## 13. Phase 2 implementation and verification

`src/conversion.h` defines owned USD snapshots and results. Capture resolves
default-time composed values, including references and unsaved local/session
opinions, on the caller's USD thread. The worker receives only numeric buffers;
it never receives a stage, prim, layer, attribute, Qt object, or Python object.

The private `mesh_api.h` C ABI validates and round-trips those buffers through a
real BMesh. Named CustomData layers carry float attributes and categorical integer
source-point, source-face, source-corner, and material IDs. Layer offsets are
resolved after the complete layout is built, because adding a different layer
type can relocate earlier layers. Both the probe and conversion share one runtime
mutex and perform initialization, allocation, freeing, and teardown on a joined
worker. Successful conversion checks that guarded allocations return to baseline.

Supported data in this phase:

| Data | Conversion behavior |
|---|---|
| Polygon geometry | Preserve points, polygon order, winding, loose points, disconnected components, and boundaries; non-manifold edges can round trip |
| Orientation | Reverse left-handed loops into BMesh and restore them on output, including corresponding corner data |
| Float primvars | Scalar, float2, float3, and float4 array storage, retaining exact USD roles such as texCoord2f, color3f, and color4f; vertex/varying, uniform, and faceVarying domains |
| Indexed primvars | Validate indices before expansion; round-trip domain values through BMesh and retain the original USD values/indices after verifying exact equality |
| Constant primvars | Preserve their USD opinions without treating them as topology-dependent data; includes inherited constants |
| Normals | Preserve constant, vertex/varying, uniform, and faceVarying normals as explicit float channels |
| Materials | Carry categorical face IDs for non-overlapping/partition face materialBind subsets; retain subset paths, relationships, and metadata |
| Other USD opinions | Keep transforms, material networks, references, stage units/axis, and unrelated metadata on their original prims/layers |

Capture rejects subdivision cages, authored holes/creases/corners, velocity or
acceleration arrays, time-sampled geometry/attributes, value clips, skin/blend-shape
bindings, instances/proxies, variant-composed geometry, and mapped/non-local or
read-only edit targets. Topology-dependent integer, double, half, matrix, and other
unregistered primvar types, elementSize other than one, and unauthored-value slots
are unsupported. Unknown subset families, overlaps, out-of-range membership, and
incomplete partitions are rejected.

Numeric validation rejects empty meshes, invalid polygon counts/indices, repeated
vertices within a face, duplicate polygon vertex sets, zero-length edges,
zero Newell-area faces, non-finite converted data, and invalid attribute cardinality.
Limits are 1,000,000 points/faces, 4,000,000 corners, 4,096 corners per face, 64
converted channels, and a conservative 256 MB float-storage budget, checked before
indexed expansion. These bounds do not represent total BMesh memory consumption or
guarantee that arbitrary self-intersecting/non-planar meshes are suitable for later
operations. Those operations must enforce their own geometric preconditions.

`roundTrip` validates returned geometry, attributes, and identity provenance maps.
At the phase 2 checkpoint, `resultLayer` created a new anonymous override containing geometry, recomputed
extent, supported primvars/normals, and subset indices. It has no API for authoring
into the source stage. The strict round-trip API still rejects topology changes;
section 14 adds a separate operation API and transactional authoring.

The optional `usdcc_blender_mesh_conversion` CTest entry checks composed referenced
assets, unsaved overrides, both orientations, UV seams, indexed RGB/RGBA colors,
all supported interpolation domains, normals, material networks/subset metadata,
reflected non-uniform transforms, n-gons, loose points, non-manifold edges, and
concurrent probe/conversion calls. Repeated round trips and scratch-layer composition
preserve source/root/session layer content. Unsupported cases and corrupt results
are rejected without authoring. The native harness, plugin lifecycle tests, source
build-tool tests, and full scripting/plugin regression suite also pass on Windows.

Use the commands in section 12 to rebuild the backend, enable the option, and run
all `usdcc_blender_mesh` checks. The build inventory now includes the conversion
ABI version and adapter/header hashes. The source and packaging license policy is
unchanged. Section 14 implements operation-sized output buffers, the persistent
executor, cancellation/stale-result checks, and live undoable commands.

## 14. Editing commands and transactions (phases 3–4)

The **Mesh** main menu and Outliner **Mesh** submenu expose all six commands.
Dialogs retain parameters through the plugin's settings namespace. Distances are
mesh-local; angles are degrees in the dialog. Bevel offers named existing material
subsets for a single mesh and adjacent-face material assignment for a batch.
Progress is non-modal, with Cancel, and outcomes go to the log/toast system.

The app captures all selected meshes before launching one batch. The background
dispatcher receives only `MeshData`, parameters, and an owned atomic cancellation
token. The private C API returns immutable, variable-sized result buffers through
an opaque handle; the app copies them before freeing the handle in the backend.
No Blender allocation is freed by usdcc. The persistent native executor owns
initialization and teardown. It can shut down and restart for plugin activation
cycles. Source extraction now includes the real operator engine and an explicit
three-operator GPL registry, plus native collapse/dissolve tools. The only registry
changes are explicit enum casts required by strict C++20 aggregate initialization.

Before direct tools run, the backend ensures operator flags and element indices,
which Blender's normal modifier/operator callers otherwise supply. Generic custom
data offsets are resolved after the complete layer layout. Guarded allocation
diagnostics and net block changes fail the operation. Conversion polls cancellation
between bounded construction batches; unsafe native kernels are allowed to finish
before results and resources are discarded. Shutdown cancels and joins outstanding
work and stops the persistent worker.

| Command | Implemented contract and constraints |
|---|---|
| Triangulate | Five quad methods and beauty/ear-clip n-gons; exact point/face/corner attribute provenance is checked |
| Merge by Distance | Native find-doubles/weld with exact vertex/corner/uniform/material boundary signatures; target point data is retained, not averaged; protected matches may prevent a requested weld |
| Limited Dissolve | Native angle-limited dissolve; boundary geometry and seams/sharp/material/uniform-data discontinuities are protected |
| Recalculate Normals | Native consistent/outside winding where possible, then flat face-varying normals; open/non-manifold outside orientation is reported as ambiguous |
| Decimate | Closed manifold geometry without protected UV, sharp-normal, material, or uniform-data boundaries; target is relative to original polygon count; outputs triangles, keeps a minimum four-triangle target, and reports the achieved count; impossible/invalid results are rejected |
| Bevel | Eligible manifold edges of a closed mesh; width, segments, profile, angle filter, overlap clamp, and existing/adjacent material policy; tested corner UV islands and point colors survive interpolation |

Topology-changing commands regenerate flat face-varying normals (including
`primvars:normals` when present), recompute extent, write domain-consistent primvar
arrays, and block stale indexing. Material subset indices are rebuilt using
categorical face membership; relationships and unrelated metadata remain in place.
New elements without reliable original provenance use `-1`, rather than fabricated
source IDs. The operation API validates geometry, channel schemas, source-map
ranges, material IDs, and finite output before preparing any live authoring.

On the UI thread, completion checks stage identity/open state, captured edit target,
stage-change notices, and a fresh composed snapshot. Any stage edit is conservatively
stale. Switching the current stage alone does not redirect the result. A fresh
`UndoManager::tryBeginEdit` refuses nesting or another stage/manager's active
recording of a shared layer. Authoring copies only geometry attribute fields from
a validated scratch layer, verifies their composed values/metadata, and rolls the
whole selection back if any authoring or composition check fails. Redo replays the
recorded fields. No-op, failed, stale and cancelled jobs create no undo entry.

Tests cover actual menu/dialog dispatch, exact undo/redo including newly created
attributes, a no-op, selection deduplication, cancellation, stage closure, edits
during work, target changes, stage switching, shutdown, shared-layer recording
conflicts, and full batch rollback when stronger opinions hide a result. Native
fixtures cover UV/material/color boundaries, both windings, all triangulation
methods, merge/dissolve, outward cube normals, decimation, bevel segments/angle
filter/material override, and preservation of all six original cube UV islands.

## 15. Distribution candidates, verification and release gate (phase 5)

Every developer and CI build includes mesh tools. `tools/build.py` builds the
backend explicitly, uses bounded parallelism (`--jobs`, default up to four), then
builds the app. Both the source archive and native package are pinned and their
inputs are inventoried. Generated source files are written only when content changes,
so subsequent builds reuse unchanged objects. The backend matches Blender's unsigned
char requirements (`/J` or `-funsigned-char`); Linux also uses its aliasing/FP flags.
The Linux build exposed and fixed GCC's rejection of the nested `StageOpener::Options`
default argument by using an equivalent overload. The prerelease version regression
check now accepts a prerelease base followed by development build metadata.

There are two deployment choices:

```powershell
# Standard artifact; mesh tools are built but excluded from the package.
python tools/deploy.py build/standard-package --omit-rtx

# Separate combined app/plugin candidate, runtime check, then Windows source/review.
python tools/deploy.py build/blender-mesh-package --with-blender-mesh-tools
python tools/ci/blender_mesh_smoke_test.py --deploy build/blender-mesh-package --platform-plugins build/vcpkg_installed/x64-windows/Qt6/plugins/platforms
python tools/blender_mesh_package.py --usd-source C:/path/to/OpenUSD --package build/blender-mesh-package --fetch
python tools/blender_distribution.py build/blender-mesh-package --finalize --usd-source C:/path/to/OpenUSD --source-bundle build/blender-mesh-corresponding-source.tar.gz
python tools/blender_distribution.py build/blender-mesh-package --verify
```

On Linux, omit `--platform-plugins`; the package has its own offscreen plugin.
The combined option excludes RTX, V-Ray and stale third-party USD renderers using
an explicit reviewed USD plugin allowlist. Plugin-private libraries
stay inside its bin directory; Linux deployment excludes those from the global
app/SDK library scan, repairs `$ORIGIN` RUNPATHs, and checks `ldd` resolution.
The standard artifact excludes both source and stale compiled Blender output.

The source candidate contains the working app snapshot (including local changes),
exact verified Blender archive and generated GPL excerpts, OpenUSD checkout at the
pinned revision, local OpenUSD dependency/bundled source trees, PySide source when
available, and vcpkg source resources verified with SPDX SHA-512 records. Git-hosted
resources are matched to the download cache by hash, not guessed filenames. Agent
configuration and unrelated untracked private files are excluded. A JSON sidecar
records per-project-file hashes, archive hash, source-resource status, and review
state. Supplemental sources include exact CPython/externals, complete TBB sources,
Ruff/uv and all Cargo.lock registry dependencies, with notices. Self-contained Git
bundles preserve dependency build-system identity. This is source evidence for review, not an assertion that a list of SPDX
resources alone proves complete corresponding-source delivery.

Deployment starts with a pending `distribution-readiness.json`. Windows finalization
checks the reviewed policy, binary/source correspondence, source resources, backend
inputs, runtime origins/imports and required notices. It installs the actual OpenUSD
TOST LICENSE.txt/NOTICE.txt, GPL terms at the package root, attribution materials and
accompanying source, then hashes the whole deliverable. Verification rejects changed
or added files. See [the review](BLENDER_DISTRIBUTION_REVIEW.md) and
[source/replacement instructions](BLENDER_SOURCE_BUILD.md). Linux has a separate
[Ubuntu source/dependency review](BLENDER_LINUX_DISTRIBUTION_REVIEW.md) and
[source/replacement instructions](BLENDER_LINUX_SOURCE_BUILD.md).
`tools/blender_linux_distribution.py` collects, finalizes and verifies its Linux
artifact using a separate lock. It cannot pass using the Windows receipt.
No binary has been published. The optional manual
`mesh_distribution_checks` input in `build.yml` builds candidates, runs smoke tests
and benchmarks, and uploads verification reports only.

Local checks pass for the full Windows regression suite and Ubuntu 24.04/GCC 13.3
under WSL/Xvfb. Packaged checks use isolated profiles and PATHs, verify the backend
was loaded from the package, exercise allocator probes, and, when PySide is present,
run the real triangulate menu plus undo/redo before shutdown. Linux's package has
no PySide yet, so its packaged script checks native activation/probes; the C++
integration tests exercise its menus. These are isolated-runtime tests on the
development machines, not a fresh OS VM or coverage of every Linux desktop.

### Performance baseline

`python tools/benchmark_blender_mesh.py` records snapshot, native import/algorithm/
export, combined backend validation/copying, scratch preparation, commit/verification,
total time and peak process working set. It runs Recalculate Normals on open triangular
grids and records actual counts. Windows 11, Ryzen 9 9950X3D (16 cores/32 threads),
MSVC 19.44.35229.0, RelWithDebInfo, the pinned backend, one serialized algorithm worker:

The harness includes USD authoring and composed verification; it excludes Hydra
viewport redraw and the options/progress UI.

| Actual faces | Points | Snapshot ms | Import ms | Algorithm ms | Export ms | Backend + validation ms | Prepare ms | Commit + verify ms | Total ms | Peak process MiB |
|---|---|---|---|---|---|---|---|---|---|---|
| 9,800 | 5,041 | 8.9 | 2.2 | 2.7 | 1.1 | 20.1 | 6.3 | 8.8 | 44 | 62 |
| 99,458 | 50,176 | 51.6 | 17.5 | 54.6 | 15.8 | 196.4 | 46.3 | 97.0 | 391 | 129 |
| 999,698 | 501,264 | 614.5 | 178.8 | 631.4 | 235.9 | 2,308.4 | 778.4 | 1,595.9 | 5,297 | 780 |

Import/algorithm/export are components of the backend column, not additional total
time. The largest cancellation probe, requested 10 ms after starting the worker,
returned without authoring in about 289 ms during preflight/import. Cancellation
during an unsafe native kernel waits for that kernel. These are workload-specific
observations, with concurrent system activity and no real-time or all-operation
performance guarantee. The approximately 0.6 s largest UI snapshot/validation cost
is a current limitation; component previews and interruptible algorithms remain
outside this first release.
