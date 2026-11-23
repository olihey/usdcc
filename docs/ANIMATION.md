# Animation — Plan

Status: **all five phases built**: playback, keyframing, key editing, curves and rendered output
(§6.1, §6.3 to §6.6).
The choices are in §4, and §8's questions are answered as recommended.

Before this, usdcc had no notion of time: every read and write was at `UsdTimeCode::Default()`,
the viewport rendered `params.frame = Default`, and the Attribute Editor disabled time-sampled rows
"until a timeline exists" ([ATTRIBUTE_EDITOR.md](ATTRIBUTE_EDITOR.md) §4.1, §10). This plan adds
time in five phases: playback, keyframing, key editing, curves, and rendered output.

## 1. What OpenUSD offers

Checked against the headers of the build the app links (`USD_INSTALL`, which reports 26.08;
[PLAN.md](PLAN.md) §5 says 26.11 — both have everything below).

| Mechanism | What it is | Value types | Who else reads it |
|---|---|---|---|
| **Time samples** | Values at time codes, per attribute. Between them: linear or held, chosen per *stage* at runtime (`UsdStage::SetInterpolationType`), not stored in the file | All. Linear only for numeric types; the rest are held | Everything that reads USD |
| **Splines** (`TsSpline`, `UsdAttribute::GetSpline/SetSpline`) | Real animation curves: Bezier or Hermite knots with tangents, held/linear/curve segments, pre- and post-extrapolation (held, linear, sloped, looping), inner loops, `Breakdown()`, anti-regression | **Scalar `double`, `float`, `half` only** | Only readers on a recent OpenUSD; older ones see no animation |
| **Scalar transform ops** (`xformOp:translateX/Y/Z`, `scaleX/Y/Z`, next to the old `rotateX/Y/Z`) | What lets a transform be spline-driven, since `translate` is a `double3` and can't hold a spline | — | Older readers don't know the op types |
| **Stage time metadata** | `startTimeCode`, `endTimeCode`, `timeCodesPerSecond`, `framesPerSecond`; layer offset and scale on sublayers and references | — | Everything. Already editable in Layer Properties |
| **Value clips** | Time samples pulled from other files per time range (crowds, sims, per-frame caches) | All | Everything; composes without any code of ours |
| **UsdSkel** | Skeletons, `SkelAnimation` (joint transforms, blend-shape weights as time samples), skin bindings. `usdSkelImaging` is in the build and skins on the GPU in Storm | — | Widely supported |
| **Blocks** | `Block()` (no value), `BlockAnimation()` (default shows through, animation hidden), time-varying blocks | — | — |
| **`UsdTimeCode::PreTime`** | The value just before a time, for held steps and spline discontinuities | — | — |
| **OpenExec** (`exec`, `execUsd`, `execGeom`, `execIr`, `usdExecImaging` are in the build) | Pixar's computation engine for computed values and rigging | — | Too young to build on; see §9 |

Two rules of value resolution shape everything in §4:

- **Within a layer, animation beats the default.** `attr.Set(v)` on an attribute that has time
  samples in the same layer changes nothing you can see at any numeric time.
- **Time samples don't merge across layers.** The strongest layer that has *any* sample for an
  attribute supplies *all* of them. One key authored in a stronger layer replaces the whole
  animation below it with a constant; a *default* authored in a stronger layer hides it entirely.

## 2. What there was before phase 1

| Where | State |
|---|---|
| `Stage` ([stage.h](../src/cpp/usd/include/usdcc/usd/stage.h)) | Holds per-stage app state (selection, target op, edit target). No time |
| Viewport | `params.frame = Default`, in the main engine and the `SelectionOverlay` engine. It redraws on `requestUpdate()`; no clock. The HUD keeps a slot for the time code ([VIEWPORT_HUD.md](VIEWPORT_HUD.md)) |
| Attribute Editor | Reads at `Default`, falling back to the earliest sample. Time-sampled rows are read-only with an "animated" badge ([attribute_binding.cpp](../src/cpp/usd/src/properties/attribute_binding.cpp)) |
| Move/Rotate/Scale | Read and write at `Default` ([transform_gizmo_tool.cpp](../src/cpp/tools/src/transform_gizmo_tool.cpp)). **On an animated prim the drag writes a default the animation hides, and reports success** |
| Reparent | Skips keeping the world position when a prim or parent is animated ([OUTLINER_REPARENT.md](OUTLINER_REPARENT.md)) |
| Undo | The layer state delegate already records `SetTimeSample` and every field, so time samples and splines (one field) are undoable today ([undo_manager.cpp](../src/cpp/usd/src/undo_manager.cpp)). Only fields coalesce during a drag; time samples don't yet |
| Layer Properties | Start, end, time codes per second, frames per second, sublayer offset and scale: all editable |
| Create menu, icons, relationships | `SkelRoot`, `Skeleton`, `SkelAnimation`, `BlendShape` can be made; `skel:animationSource` can be set |
| Render plugins | RTX renders "USD default time", V-Ray renders frame 0, the Render View reads cameras at the default time |
| Scripts | Can author time samples through `pxr`, with undo. Nothing shows them |

About 40 call sites pass `UsdTimeCode::Default()`. They fall into three groups (§5.2).

## 3. What usdcc could offer

| Tier | What the user gets | Cost |
|---|---|---|
| **A. Playback** | A timeline: scrub, play, stop, loop, step, go to start/end, frame field, range, fps. Everything time-varying shows: transforms, visibility, cameras, lights, material inputs, deforming meshes, value clips, splines, skeletons and blend shapes. The Attribute Editor and gizmos show values at the current time | Small. USD and Hydra do the work; we supply the clock and pass the time through |
| **B. Keyframing** | Set and remove keys on any attribute, auto-key, gizmos that key, key marks on the timeline for the selection, next/previous key, a key state on each attribute row | Medium. The policy for *where* an edit goes is the real work (§4.4) |
| **C. Key editing** | A dope sheet: move, scale, copy, paste and delete keys across prims and attributes | Medium |
| **D. Curves** | A curve editor on USD splines for scalar attributes: knots, tangents, extrapolation, loops; convert samples to a curve and bake back | Large |
| **E. Output** | The render plugins render the current time; a sequence render; a viewport playblast | Medium, per renderer |
| Not offered | Rigging, constraints, IK, joint posing, retargeting, simulation, audio | See §9 |

Tier A alone makes usdcc usable for reviewing animated assets and shots, which it can't do at all
today. Tier B is what makes it an animation *tool*. Tiers C to E are refinements on top.

## 4. Directions

### 4.1 Time belongs to the stage

**Each `Stage` has one current time. Not one per viewport, and not one for the whole app.**

- Stages are independent documents ([PLAN.md](PLAN.md) §5 item 8) with their own range and frame
  rate. One global time would be wrong for all but one of them.
- Every panel on a stage must agree: the value in the Attribute Editor has to be the one the
  viewport draws and the one a gizmo drag starts from. Per-viewport time breaks that.
- `Stage` exists to hold exactly this kind of state, and it is Qt-free, so tools, Python and
  plugins can all read it.

The time is view state: not undoable, never written to a layer, saved in the session.

### 4.2 One transport, on the current stage

**One timeline bar for the app, driving `StageManager::currentStage()`** — the stage of the view
panel last focused ([CURRENT_STAGE.md](CURRENT_STAGE.md)). Switching stage shows that stage's
time and range. Playing stops when the current stage changes.

This is the "global tool acts on the current stage" rule that Undo and Save already follow. A
transport per stage would mean several clocks and several Play buttons.

### 4.3 The timeline is a fixed bar, not a panel

**A thin bar under the dock area, above the bottom side panels. View > Timeline hides it.**

- Not a `ViewPanel`: those are stage-bound, can be tabbed behind others or closed, and there can
  be many. A transport must be single and always reachable.
- Not a `SidePanel`: those tab with the Log and Script Editor and collapse with them. The
  timeline has to stay visible while the Log is open.

The dope sheet and curve editor (tiers C, D) *are* `ViewPanel`s: large, per-stage work surfaces.

### 4.4 Keys are time samples; curves are opt-in

**A key is a time sample. Splines come later, per attribute, when the user asks for a curve.**

| | Time samples | Splines |
|---|---|---|
| Value types | all | scalar floating point only |
| Transforms | work on the ops prims already have | need `translate` split into `translateX/Y/Z` |
| Other readers (RTX, V-Ray, Houdini, Blender, usdz) | all | recent OpenUSD only |
| Interpolation | linear or held, stage-wide | per-segment, tangents, loops |
| Authoring cost | one sample per key | whole spline rewritten per edit |

A file usdcc writes has to animate in the renderers we ship plugins for and in other DCCs.
Time samples always do. Splines are the better *authoring* form for ease-in/out and loops, so
they are worth having, but as a per-attribute choice with **Bake to Samples** as the way out.
Splines that already exist in a file play back from phase 1 at no cost.

### 4.5 One rule for where an edit goes

Every write that can touch an animated attribute goes through one helper (§5.3) with this rule:

| The attribute | Auto-key off | Auto-key on |
|---|---|---|
| Not animated | Sets the default, as today | Sets a key at the current time |
| Animated | **Sets a key at the current time** | The same |

- Keying an animated attribute even with auto-key off is the only edit that shows (§1, first
  rule). The alternatives are refusing the edit or writing a default that does nothing, which is
  today's silent gizmo bug.
- **Animation from a weaker layer is copied into the edit target before the first key** (§1,
  second rule), and the log says so. Otherwise one key would flatten the animation below it.
- A spline-driven attribute gets a knot instead of a sample.
- Edits to a non-animated attribute stay defaults by default, so files stay as small and plain
  as they are today for people who never animate.

### 4.6 Always read at a numeric time

Display reads use the stage's time, never `Default`. At a numeric time USD already falls back to
the default for attributes with no animation, so one rule covers both cases and the "default,
else earliest sample" fallback in `AttributeBinding::currentValue()` goes away. A stage starts at
its `startTimeCode`, or 0 without one, as usdview does.

### 4.7 The clock is on the UI thread

A `QTimer` at the frame rate advances the stage's time; viewports redraw through the
`requestUpdate()` they already use, which coalesces. Two modes: **Every Frame** (never skips; as
slow as the scene) and **Real Time** (jumps to where wall-clock time says it should be). No
render thread: Hydra is driven from the UI thread everywhere in this app, and a playback thread
would have to fight that.

## 5. Design

### 5.1 New pieces

| Piece | Target | What it does |
|---|---|---|
| `Stage::time()`, `setTime()`, `timeChanged` | `usdcc_usd` | The current time code and its signal. Separate from `changed`: a time change is not a stage edit |
| `time_range.h` | `usdcc_usd` | A stage's range (authored start/end, else a default playback range kept on the `Stage`), frame rate (`timeCodesPerSecond`, 24 without), snapping |
| `anim_edit.h` | `usdcc_usd`, Qt-free | §5.3 |
| `PlaybackController` | `usdcc_usd_ui` | The clock: play, stop, loop, direction, mode, on the current stage |
| `TimelineBar` | `usdcc_usd_ui` | Transport buttons, frame field, range fields, fps, the ruler with the selection's key marks, auto-key toggle |
| `MainWindow::setBottomBar(QWidget*)` | `usdcc_ui` | Wraps the dock manager and the bar in one central widget ([main_window.cpp:138](../src/cpp/ui/src/main_window.cpp)); knows nothing about time |
| `DopeSheetViewPanel`, `CurveEditorViewPanel` | `usdcc_usd_ui` | Phases 3 and 4, registered like the other view panels |

### 5.2 The `Default()` call sites

| Group | Sites | Change |
|---|---|---|
| **Display** | viewport and `SelectionOverlay` render params, `sceneBounds` and Fit to View, HUD, gizmo frame (`ComputeLocalToWorldTransform`), Render View camera, `xform_math` | Take the stage's time |
| **Authoring** | `AttributeBinding`, `TransformGizmoTool` and the three tools, `xform_stack`, `shade_actions`, reparent | Go through `anim_edit` |
| **Leave** | `scene_counts`, `dependencies`, `shading_preview` (its own stage), `value_type_info` | Not about the stage's time |

`ToolContext` gains `time` and `autoKey`, so `usdcc_tools` stays free of Qt and of the app.

### 5.3 `anim_edit`

```cpp
enum class KeyState { Static, Animated, KeyedHere };   // for the row badge and the timeline
KeyState keyState(const UsdAttribute&, UsdTimeCode);
bool setValue(const UsdAttribute&, const VtValue&, UsdTimeCode, bool autoKey);   // the §4.5 rule
bool setKey(const UsdAttribute&, UsdTimeCode);      // keys the value it has now
bool removeKey(const UsdAttribute&, UsdTimeCode);
bool removeAnimation(const UsdAttribute&);          // back to a default: the value at this time
bool localizeAnimation(const UsdAttribute&);        // weaker layer's samples into the edit target
std::vector<double> keyTimes(const std::vector<UsdAttribute>&);
```

All of it uses `UsdAttribute`, not `Sdf`, so the edit target's layer offset is applied to times
for us. Callers wrap calls in the `UndoManager` sessions they already open.

### 5.4 Staying fast while playing

- Hydra only re-reads prims it knows are time-varying when the frame changes. Nothing to do.
- The Attribute Editor refreshes only rows whose `ValueMightBeTimeVarying()`, and at most about
  ten times a second while playing.
- The timeline's key marks are recomputed on selection or stage change, not per frame.
- A progressive renderer (Embree, a path tracer) restarts on every frame; that is expected.

### 5.5 Build or reuse the timeline widgets

**Decided: build them ourselves, and copy no code from OpenDCC or anywhere else.** No
permissively licensed Qt library offers a dope sheet that could be dropped in, and the one
usable source of timeline code was turned down.

| Candidate | License | Verdict |
|---|---|---|
| OpenDCC `timeline_widget` (ruler with key marks, range slider, time selection, about 3,000 lines) and `graph_editor` (curve editor and channel list, about 4,500 lines) | Apache-2.0, Qt 6 | The licence would allow it, but **not used: no OpenDCC code is copied.** It has no dope sheet anyway, and its curve editor is written against OpenDCC's own curve engine, not `TsSpline` |
| `qt-animation-timeline` (PyPI) | BSD-3 | A Blender-style track and keyframe widget, but Python, very young, and it owns its keyframes and writes them to Python attributes. Not a base for a C++ panel on USD data |
| O3DE Track View (dope sheet, curve editor) | Apache-2.0 or MIT | Qt 5 and tied to O3DE's sequence classes; porting costs more than writing |
| Gaffer's animation editor, OpenToonz's function editor | BSD-3 | Drawn with Gaffer's own GL gadgets; tied to Toonz's data. Reference only |
| Natron, Qt Creator / Design Studio timeline and curve editor, Qt Quick Timeline, Blender, Krita | GPL | Can't be linked into a BSD-3 app |

The widget is the small part: a ruler, rows, diamonds, box select and drag, drawn with
`QPainter` over one shared time axis. The work is the model (which attributes, their key times,
following stage changes) and the edits going through `anim_edit` and undo, which no library has.
The ruler, the dope sheet and the curve editor must share one time-axis class (zoom, pan, snap),
which also argues for owning all three.

What we write, all in `usdcc_usd_ui`, custom-painted with `QPainter` like the rest of the app:

| Widget | Phase | What it is |
|---|---|---|
| `TimeAxis` | 1 | Not a widget: time to pixel and back, zoom, pan, frame snapping, tick spacing. Plain data, unit-tested without a window |
| `TimeRuler` | 1 | Ticks, frame labels, the playhead, scrubbing. Key marks in phase 2; dragging and deleting keys in phase 3 |
| `RangeSlider` | 1 | The playback range inside the stage range, two handles and a bar |
| `DopeSheetView` | 3 | Rows from a model (prim, then attribute), key diamonds, box select, move and scale in time. Rows are virtualized: only those on screen are painted |
| `CurveView` | 4 | Curves sampled from `TsSpline`, knots and tangent handles, value axis with its own zoom |

The row list beside the dope sheet and curve editor is a `QTreeView` on the same model, so
expanding, selecting and scrolling come from Qt.

## 6. Phases

Each phase ships on its own and leaves the app consistent.

| Phase | What | Main files | Tests |
|---|---|---|---|
| **1. Playback** | `Stage` time; `PlaybackController`; `TimelineBar` (no key marks); the Display group of §5.2; Attribute Editor rows show the value at the current time, still read-only when animated; HUD time code; session saves time, range, loop; Python: `usdcc.usd.current_time()`, `set_current_time()`, `time_range()`, `usdcc.app` play/stop and a time-changed callback | `stage.h/.cpp`, `viewport_render_params.cpp`, `hydra_viewport_window.cpp`, `selection_overlay.cpp`, `attribute_binding.cpp`, `main_window.cpp`, `main.cpp`, bindings | Stage time and signal; range and snapping; the controller against a fake clock (loop, both modes); the viewport draws two times differently for a time-sampled, a spline-driven and a skinned prim; a non-animated stage draws as before |
| **2. Keyframing** | `anim_edit`; the Authoring group of §5.2; row key button (static / animated / keyed here) and menu (Set Key, Remove Key, Remove Animation); Set Key shortcut for the selection's transform; auto-key toggle; key marks, next/previous key; Held/Linear stage switch; undo coalesces repeated samples at one time; reparent keeps world position on animated prims at the current time | `anim_edit.h/.cpp`, `attribute_binding.cpp`, `property_sheet.cpp`, `transform_gizmo_tool.cpp`, `xform_stack.cpp`, `undo_manager.cpp`, `tool.h` | The §4.5 table case by case; localizing from a weaker layer; a sublayer with an offset keys at the right time; a gizmo drag on an animated prim is one undo step and shows; Remove Animation keeps the pose |
| **3. Key editing** | Drag, delete, copy and paste keys on the timeline ruler; `DopeSheetViewPanel` with rows per prim and attribute, box select, move and scale in time | new panel, `anim_edit` | Moving keys keeps values; scaling about a pivot; one undo step per gesture |
| **4. Curves** | Convert to Curve / Bake to Samples for scalar attributes; Split Translate/Scale into X, Y, Z (with the compatibility warning); `CurveEditorViewPanel`: knots, tangents, segment type, extrapolation, loops; `anim_edit` writes knots on spline attributes | new panel, `xform_stack.cpp`, `anim_edit` | Convert and bake round-trip within tolerance; split ops give the same matrix at every sample; a tangent drag is one undo step |
| **5. Output** | The Render View hands the stage's time to its renderer; RTX and V-Ray render it; Render Sequence over the range; viewport playblast to an image sequence from `grabFrame()` | `render_view_panel.cpp`, the two plugins, [PLUGIN_SDK.md](PLUGIN_SDK.md) | A moving camera renders differently at two times; a sequence writes one file per frame |

### 6.1 Phase 1: what was built

**Try it:** `python tools/run.py --scene test_data/animated.usda`. A cube hops (time samples), a
sphere slides (a spline), another pulses (a time-sampled radius).

| Piece | Where | Notes |
|---|---|---|
| Stage time and playback range | `Stage::time()`, `setTime()`, `timeChanged`; `playbackRange()`, `setPlaybackRange()`, `clearPlaybackRange()` | Starts at the stage's `startTimeCode`, 0 without one. Neither sends a change notice nor dirties a layer |
| Range, frame rate, snapping | `time_range.h` | `stageTimeRange()` is 0 to 100 without both time codes; `timeCodesPerSecond()` is 24 without one |
| Where playback goes next | `playback.h` | `nextFrame()` (Every Frame), `frameAfter()` (Real Time), `steppedFrame()`. Plain arithmetic, no Qt |
| The transport | `PlaybackController` | On `StageManager::currentStage()`; stops when that changes. Loop and the mode are kept in usdcc.ini (`Timeline/loop`, `Timeline/everyFrame`), not in the session: they are the user's, not the stage's |
| The widgets | `TimeAxis`, `TimeRuler`, `TimeRangeSlider`, `TimelineBar` | Custom-painted. Scrubbing snaps to frames; Shift scrubs between them. The two range fields push each other so the range is never under a frame. The bar's menu: Loop, Play Every Frame / in Real Time, Reset Playback Range, Set Stage Range to Playback Range (one undo step) |
| One scale | `TimelineBar::shownRange()` | **The ruler and the slider show the same times**, so the slider's bar lies under exactly the frames that play, and the ruler dims the rest. What's shown is the stage's own range with the playback range, or the playback range alone for a stage that has none. Dragging the slider leaves it alone, so a range can be made shorter and longer again; anything else that sets the playback range fits it anew. Scrubbing reaches every frame shown, playing or not |
| The bar's place | `MainWindow::setBottomBar()` | The dock manager and the bar share the central widget. View > Timeline; kept in the saved layout and in a session's layout (`bottomBarVisible`) |
| Viewport | `HydraViewportWindow::setTime()` | The frame, picking, the selection overlay and Fit to View use it. A viewport nobody gave a time (the Material Editor's preview) stays at the default one |
| HUD | bottom right: `Frame 24` | With the HUD itself; no switch of its own |
| Gizmos | `ToolContext::time` | The gizmo sits where the prim is at the time shown. A drag still writes the default (phase 2) |
| Attribute Editor | `AttributeBinding`, `PropertySheet` | Values at the stage's time; rows that vary are read again 50 ms after the time moves. Spline-driven rows count as animated too. Still read-only (phase 2) |
| Session | `session_stages` | Per stage: `time`, and `playbackRange` when it has its own |
| Python | `usdcc.usd.current_time()`, `set_current_time()`, `time_range()`, `playback_range()`, `set_playback_range()`; `usdcc.app.play()`, `stop()`, `is_playing()`; the `time_changed` event | |
| Shortcuts | Settings > Shortcuts > Timeline | Play or stop: Ctrl+Space. Next and previous frame: `.` and `,`. Not plain Space, which ticks a row's check box in the Outliner |
| Icons | `tools/gen_theme_icons.py` | skip-start, skip-end, step-back, step-forward, loop |

Changed on 2026-11-16: the ruler first showed only the playback range, zoomed to it, over a slider
on the whole stage range. Two scales one above the other read as one: the bar seemed to mark frames
it didn't, and playback seemed to run past it. Now both are on one scale. What that costs is
precision on a long stage, until the ruler can be zoomed (`TimeAxis::zoom()` is there; phase 3).

Left out of phase 1, against the plan above:

- **The Render View's camera** stays at the default time until its renderer draws the same time
  (phase 5); a camera at one time over a scene at another would be worse than both at the default.
- **Workspaces** ([WORKSPACES.md](WORKSPACES.md)) don't say whether the timeline shows: it is not a side
  panel, so a workspace leaves it as it is. A `timeline` key there would be the place.

Tests: `stage_time_test.cpp` (stage time, ranges, playback steps, the time axis),
`time_imaging_test.cpp` (what Hydra is handed at two times: time samples and a spline),
`timeline_test.cpp` (ruler, slider, transport, bar, bottom bar, viewport, Attribute Editor, session),
and `python_time` in `python_app_test.cpp`. Checked in the app with Storm on
`test_data/animated.usda`: scrubbing and playing at 24 fps, the HUD's frame, the animated row.

### 6.3 Phase 2: what was built

**Try it:** open `test_data/animated.usda`, select the Hopper, go to frame 10 and drag it, or
press S: a key at 10, the ruler's diamonds and the Attribute Editor's row keys say so.

| Piece | Where | Notes |
|---|---|---|
| The rule | [anim_edit.h](../src/cpp/usd/include/usdcc/usd/anim_edit.h), `usdcc_usd`, Qt-free | §5.3's functions, plus `animatedAttributes()` and the app-wide `autoKey()` / `setAutoKey()` / `autoKeyChanged()`. A static attribute gets its default unless auto-key is on; an animated one gets a key either way (§4.5). A spline-driven attribute gets a knot, keeping that knot's tangents when there is one already. Through `UsdAttribute`, so a sublayer's offset applies (checked) |
| Localizing | `localizeAnimation()` | A weaker layer's samples (or spline) are copied into the edit target before the first key, read all first: each one written changes what the rest read. Logged |
| Removing | `removeKey()`, `removeAnimation()` | The last key gone, its value stays as the default. Remove Animation clears the edit target's samples and spline and sets the default to the value at the frame shown; that default also holds still a weaker layer's animation, which is left as it is |
| Attribute Editor | `AttributeBinding`, `PropertyRow`, `PropertySheet` | Animated rows are editable now, and an edit keys them. Every attribute row (the transform stack's too) has a key in front of its label, taking its width from the label so rows are no wider: ◇ grey (static), ◇ in the accent (animated), ◆ (a key at this frame). A click sets a key, or removes the one here; the menu has Set Key, Remove Key, Remove Animation, each one undo step |
| Gizmos | `ToolContext::autoKey`; Move, Rotate, Scale | Read and write at `ToolContext::time` through `anim::setValue()`: a drag on an animated prim keys it at the frame shown, one undo step. The viewport fills `autoKey` from `anim::autoKey()` |
| Undo | [undo_manager.cpp](../src/cpp/usd/src/undo_manager.cpp) | A run of sets of one time sample keeps only the first old value, as fields already did |
| Timeline | [timeline_bar.cpp](../src/cpp/usd/src/timeline_bar.cpp), `TimeRuler::setKeys()` | The selection's keys as diamonds on the ruler (every animated attribute of the selected prims), again 100 ms after selection or stage changes. ‹◆ and ◆› go to the previous and next key; **Auto** turns auto-key on, red while it is. The menu's **Held Between Keys** sets the stage's interpolation (`UsdStage::SetInterpolationType()`, not saved) |
| Shortcuts | Settings > Shortcuts > Timeline | Set Key (S: every transform op of the selection, one undo step), Next Key (Alt+.), Previous Key (Alt+,) |
| Reparent | `prim_actions::movePrims()` | With Keep World Position, an animated prim (or one under an animated parent) keeps its ops and gets `xformOp:transform:reparent` ahead of them, so it stays where it is at the frame shown |
| Op renames | `xform_stack.cpp` | Copy a spline too, besides the default and samples |
| Python | `usdcc.usd.set_key()`, `remove_key()`, `remove_animation()`, `key_times()`, `auto_key()`, `set_auto_key()` | By `Usd.Attribute` or path; a script's run is one undo step |

**Since:** Physics' Bake writes its frames with `anim::setSamples()`, which replaces an
attribute's animation in the edit target whole (no localizing: nothing of it is kept), through
`UsdAttribute` so the edit target's offset applies; phase 4's Bake to Samples can use it too.

Tests: `anim_edit_test.cpp` (§4.5 case by case, localizing, a sublayer's offset, splines, a drag's
keys as one undo step), `timeline_keys` in `timeline_test.cpp` (the ruler's keys, next and
previous key, Set Key, auto-key, a row's key button, an edit keying an animated row),
`prim_move_keeps_the_world_position` (reparenting under an animated parent) and `python_time`.
Checked in the app with `test_data/animated.usda`: Set Key at 10 from the window's action, the
ruler's diamonds, the rows' keys.

### 6.4 Phase 3: what was built

**Try it:** open `test_data/animated.usda`, select World, and open a Dope Sheet (the Stages
menu). Drag a key, box-pick a column of them and drag the handle at the end of the bar above
them, or press Delete.

| Piece | Where | Notes |
|---|---|---|
| Key editing | [anim_edit.h](../src/cpp/usd/include/usdcc/usd/anim_edit.h), `usdcc_usd`, Qt-free | `KeyRef`, `keysAt()`, `retimeKeys()`, `moveKeys()`, `scaleKeys()` (about a pivot, new times rounded to frames), `removeKeys()`, `copyKeys()` / `pasteKeys()` and the app's `keyClipboard()`. In stage time, through `UsdAttribute`, so a sublayer's offset applies; a weaker layer's animation is copied into the edit target first, as keying does; a spline's knots move with their tangents. All values are read before any is written. A key landing on one that stays replaces it |
| Pasting | `pasteKeys()` | Onto the attributes the keys came from; or, when they came from one prim and other prims are selected, onto those prims' attributes of the same name. Through `setValue()` with auto-key, so a static attribute gets keys |
| The ruler | [time_ruler.cpp](../src/cpp/usd/src/time_ruler.cpp), [timeline_bar.cpp](../src/cpp/usd/src/timeline_bar.cpp) | Pressing a key mark picks it (outlined in white) instead of scrubbing; dragging moves every key of the selection at that time, whole frames. With the ruler focused: Delete, Ctrl+C, and Ctrl+V at the frame shown, onto the selected prims |
| Dope Sheet | [dope_sheet_view_panel.cpp](../src/cpp/usd/src/dope_sheet_view_panel.cpp), panel type `dopeSheet` | A tree of prims and their animated attributes beside `DopeSheetView`, which paints the rows against its own `TimeAxis`, with a `TimeRuler` over it showing the same times and the playhead. The tree's header is the ruler's height, and the view finds each row's place from the tree, so they line up as it scrolls and folds |
| Rows | | The selected prims and their animated descendants, so picking a character's root shows its parts; **Show All Animated** (options menu, kept in sessions) every animated prim of the stage, at most 2,000. A prim row's keys are those of all its attributes; picking one picks them all |
| Picking and editing | `DopeSheetView` | Click, Ctrl or Shift click to add, a box over rows and times. Dragging a picked key moves them all; the bar over the picked keys' span has a handle at each end, and dragging one scales about the other. Delete, Ctrl+C, Ctrl+V (at the frame shown), Ctrl+A, F frames all keys, a double-click goes to a key's frame. Ctrl+wheel zooms the time, middle drag pans it, the wheel scrolls the rows. Every gesture is one undo step |

Not built, against the plan: dragging a box on the ruler (it picks one mark at a time), and a
scale on the ruler; both are the Dope Sheet's. The picked keys aren't shared between the ruler
and the Dope Sheet.

Tests: `anim_edit_move_keys`, `anim_edit_scale_keys`, `anim_edit_copy_paste_remove_keys`,
`anim_edit_move_knots_and_offsets`, `anim_edit_move_keys_undo` (usdcc_tests), and
`dope_sheet_rows`, `dope_sheet_edits_keys`, `dope_sheet_ruler_keys` (usdcc_ui_tests: rows,
Show All, a click and a drag, a box, scaling by the handle, copy, paste and delete, each one
undo step; the ruler's mark dragged, copied, pasted and deleted). Seen in the app with
`test_data/animated.usda`: the rows and keys against the ruler. Not tried by hand: the drags.

### 6.5 Phase 4: what was built

**Try it:** open `test_data/animated.usda`, select Hopper and Pulse, open a Curve Editor (the
Stages menu). Right-click `xformOp:rotateY` and Convert to Curve; pick a knot and drag it or its
tangent handles.

| Piece | Where | Notes |
|---|---|---|
| Curves | [anim_edit.h](../src/cpp/usd/include/usdcc/usd/anim_edit.h) | `canHaveCurve()` (a scalar float, double or half: what `TsSpline` holds), `hasCurve()`, `convertToCurve()` (a knot per sample, through its value; auto-ease tangents, or held segments when the stage holds; the samples cleared, as they'd win over the spline in one spec, which answers §7's question 2), `bakeToSamples()` (a sample a frame from the first knot to the last, and at each knot; extrapolation and loops past the knots aren't kept), `setKnotInterpolation()`, `setKnotTangents()` (Auto Ease, Flat), `setExtrapolation()`, `setInnerLoop()`, `movedKnots()` |
| Split | `splitXformOp()` | A translate or scale op becomes its X, Y and Z ops in its place in the op order, its default and samples split, the old op's own opinion removed from the edit target: the same matrix at every time (tested). The Curve Editor asks first, saying readers older than OpenUSD 25.11 don't know single-axis ops |
| Curve Editor | [curve_editor_view_panel.cpp](../src/cpp/usd/src/curve_editor_view_panel.cpp), panel type `curveEditor` | The selected prims and their animated descendants, each animated scalar attribute a curve in its own colour (in the tree too); others (a translate's `double3`) listed greyed, with Split into X, Y and Z in their menu. `CurveView`: the ruler's time axis and a value axis of its own with a labelled grid; splines drawn with `TsSpline::Sample()` to half a pixel, samples joined as the stage interpolates |
| Editing | `CurveView` | Click, Ctrl or Shift click, a box picks keys; clicking a row picks its curve's keys. Dragging moves the picked keys, whole frames in time and any value (Shift: value only). A picked knot shows its tangent handles where the segment is a curve; dragging one sets that side's slope and width. The menu: Interpolation (Held, Linear, Curve), Tangents (Auto Ease, Flat), Before the First Knot / After the Last Knot (Held, Linear, Repeat, Reset, Oscillate), Loop Between Picked Knots (an inner loop), No Inner Loop, Convert to Curve, Bake to Samples, Delete Keys. Wheel zooms both axes about the cursor (Ctrl: time, Shift: values), middle drag pans, F frames all. Each gesture is one undo step; a drag is drawn as it goes and written on release |

Not built: sloped extrapolation with a typed slope, Hermite curves, value blocks, and editing the
pre-value of a dual-valued knot. A vector attribute isn't drawn as three curves until it's split.
§7's question 3 (a spline's times through the edit target's offset) isn't checked yet: keys are
moved in stage time on the attribute, as for samples.

Tests: `anim_edit_convert_and_bake` (through every sample, and back to the same values),
`anim_edit_curve_settings` (interpolation, flat tangents, repeat and held extrapolation, an inner
loop, moved knots), `anim_edit_split_ops` (the same matrix at three times, a rotate refused), and
`curve_editor_curves` (usdcc_ui_tests: the curves, Convert to Curve, a knot dragged and a tangent
dragged, each one undo step, held, repeat, bake, and the split). Seen in the app with
`test_data/animated.usda`: the Hopper's rotateY and the Pulse's radius as curves, the translate
listed. Not tried by hand: the drags and the menus.

### 6.6 Phase 5: what was built

Final Render already rendered at a time: **Render Frame** at the frame the timeline shows, and
**Render Animation** over a range, through the render queue, with Hydra, RTX and V-Ray
([FINAL_RENDER.md](FINAL_RENDER.md) §3, §5.3). So a "Render Sequence" is Render Animation, and
this phase is the interactive Render View and a viewport playblast.

| Piece | Where | Notes |
|---|---|---|
| The Render View's time | [render_view_panel.cpp](../src/cpp/usd/src/render_view_panel.cpp) | `renderTime()` is the stage's time. A new time (coalesced to one per 30 ms while playing) calls the `timeChanged()` hook: a camera prim is sent again where it is then (`primRenderCamera()` takes a time now), and with Update on Edits on and anything animated (`stageIsAnimated()`, worked out again after edits) the stage is sent again. A still stage isn't re-sent for a new time |
| RTX | [rtx_panel.cpp](../src/plugins/rtx_render_native/rtx_panel.cpp), `stage_bridge.cpp` | The snapshot's start and end time code are the panel's time, as a final render pins its frame, so ovrtx renders the stage as it is then; transform edits are read at it too |
| V-Ray and Python renderers | `usdcc.ui.RenderViewPanel` | A `time_changed(time)` hook (by default as above), `time()` and `stage_is_animated()`. V-Ray's export pins the file's start and end time code to `time()` |
| Playblast | [playblast.h](../src/cpp/usd/include/usdcc/usd/playblast.h), the viewport's options menu, `usdcc.app.Viewport.playblast(pattern, start, end, step)` | The stage at each frame of a range, each frame taken once the viewport has drawn it at that time and its renderer has converged (`HydraViewportWindow::renderedTime()`, `isConverged()`), from `grabFrame()`: the scene with the grid and outline, no HUD. PNG, JPEG or TIFF; `####`, `<frame>`, `$F4`, or the frame before the extension. A progress dialog cancels it; the stage's time is put back after |

Not built: a playblast at a size other than the viewport's, and of a camera other than the one the
viewport looks through (look through it first). §7's question 5 (whether the RTX runtime and V-Ray
read splines and scalar ops) is still open: neither SDK is on this machine, so the RTX and V-Ray
changes are built by CI only and not run.

Tests: `render_view_panel_time` (a camera prim at two times; three quick times send an animated
stage once, with the camera where it is then; a still stage isn't sent again) and
`playblast_files_and_frames` (usdcc_ui_tests). In the app with `test_data/animated.usda`:
`Viewport.playblast()` over frames 1 to 6 with Storm wrote six PNGs with the Hopper moving.

### 6.2 The order

Phase 1 goes first because everything else needs it and it is useful alone. Phase 2 before 3 and
4 because it settles the authoring rule those panels reuse. Phase 5 depends only on phase 1 and
can move earlier if rendered output matters more than editing.

## 7. To verify while building

Believed, not yet tested here, except the first:

1. **Confirmed in phase 1** (`time_imaging_test.cpp`, and on screen): Hydra picks up a
   spline-driven attribute as time-varying, a scalar `translateX` op included.
2. In one attribute spec, time samples win over a spline. It decides what Convert to Curve must
   remove.
3. `UsdAttribute::SetSpline()` maps knot times through the edit target's layer offset, as
   `Set()` does for samples.
4. `Stage::setEditTargetLayer()` builds the target with the sublayer's offset
   (`GetEditTargetForLocalLayer`), not a bare `UsdEditTarget(layer)`.
5. The RTX runtime and V-Ray read splines and scalar transform ops. If not, phase 5 bakes them in
   the snapshot it already exports.
6. Skinned characters play at a usable rate in Storm on a real asset. Not tried in phase 1: there
   is no skinned asset in `test_data`.

## 8. Open questions

| # | Question | Recommended |
|---|---|---|
| 1 | Keys as time samples, with splines opt-in? | Yes (§4.4) |
| 2 | Editing an animated attribute with auto-key off: key it, or refuse? | Key it (§4.5) |
| 3 | A fixed bar, or a dockable panel? | Fixed bar (§4.3) |
| 4 | No authored range: what does the timeline show? | 0–100 as a playback range kept in the session; "Set as Stage Range" writes it |
| 5 | Should auto-key be per stage or app-wide? | App-wide, off at start |

## 9. Out of scope

- **Rigging, constraints, IK, joint posing, retargeting.** USD stores results, not rigs. OpenExec
  is where this will come from; it is in the build but new, and nothing here depends on it.
- **Editing `SkelAnimation` joints.** Skeletons play back (phase 1); posing them is a rig tool.
- **Reducing samples to curves**, motion paths, ghosting, animation layers, audio, video
  encoding (a playblast writes images).
- **Motion blur settings.** A renderer's own business once it gets the time.
