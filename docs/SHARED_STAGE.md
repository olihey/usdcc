# A Shared Stage

Several people editing one stage at once, each seeing the others' edits, selections and cameras.
One use of [PEERS.md](PEERS.md), and the largest.

**Status:** planned 2026-11-16 as part of PEERS.md, moved here 2026-11-17. The phase 0 spike is
done (2026-11-21, §10.1), and phases A to C are built (2026-11-22, §10.2), tested in one process;
not yet tried on two machines. "Checked" means looked at in this repository, "Measured" means the
spike tried it. Where what was built differs from the plan above, §10.2 says so.

## 1. What it is, and what it uses

The host of a room picks an open stage and **shares** it. Every member opens the same stage and
edits it; the edits appear on everyone's.

It needs nothing from `usdcc_peers` that a plugin couldn't use too:

| Need | From [PEERS.md](PEERS.md) |
|---|---|
| Edits, in one order for everyone | An ordered channel, `stage/<id>` (§7.1) |
| A joiner getting the edits so far | The channel's own snapshot (§7.2) |
| Selections and cameras | Per-peer state (§7.2) |
| A drag's values while it moves | A latest-only channel (§7.1) |
| Files a peer doesn't have | A transfer (§7.3) |
| View-only members | Who may send on the channel (§6) |

## 2. The live layer

Each member's stage gets one more layer on top: the **live layer**, anonymous and in memory, the
strongest sublayer of the session layer. While the stage is shared it is everyone's edit target.
Every edit is an opinion in it, and it is the only thing kept in sync.

Omniverse's live sessions work this way (a `.live` layer in the session layer, merged by the owner
at the end), which is the nearest prior art.

- **No file changes under anyone.** The layers below are read as they are on disk.
- **One layer to sync,** not a layer stack.
- **Leaving is clean.** Dropping the live layer gives back the stage as it was.

The cost is that edits are overrides. The rules are those of editing on any stronger layer:

- **Deleting** a prim that comes from a layer below deactivates it ([PRIM_DELETE.md](PRIM_DELETE.md)).
- **Renaming and reparenting** a prim that comes from below are refused while sharing. They would
  rewrite the layers below, or need `relocates`, which [OUTLINER_REPARENT.md](OUTLINER_REPARENT.md)
  found fragile. Prims created in the live layer move freely.
- **The edit target can't be changed** while sharing. The Layer Editor shows the live layer and why.

## 3. Everyone needs the same stage

The layers below the live layer must be equal on every peer.

- **The same path or URL for everyone** (a shared drive, or `webdav://` and `https://` assets,
  whose URLs mean the same on every machine: [REMOTE_ASSETS.md](REMOTE_ASSETS.md)). On joining,
  the host sends each used layer's identifier and a hash of its content. Differences are listed,
  and the joiner decides.
- **Different folders** on each machine: the root is located once, as an imported session's files
  are ([SESSIONS.md](SESSIONS.md) §5.1).
- **Not there at all:** the host sends the files (phase C), packed with `UsdUtilsLocalizeAsset`.
  *Not tried.*
- **Unsaved edits** in the stage's file layers: sharing asks to save first. An in-memory stage
  sends its root layer as USDA text, as a session keeps it.

## 4. What is sent

The undo recorder already sees every primitive change to a layer, before it is applied.
*Checked:* `EditRecorder` in [undo_manager.cpp](../src/cpp/usd/src/undo_manager.cpp) is the
layer's `SdfLayerStateDelegate` and gets nine calls: set field, set dictionary value, set time
sample, create, delete and move spec, push and pop child. Today it keeps the inverse of each, for
undo. For the live layer it also keeps the call itself.

- **An op** is one of those calls with its arguments.
- **A batch** is what one undo step recorded: one `apply()`, or one edit session. It is sent when
  the step ends, as an ordered message.
- **During a drag** the values so far go out as latest-only messages, at most 20 a second, so the
  others see the prim move. The batch at the end is what counts.
- **It catches everything.** The recorder sits on the layer, so edits from scripts, plugins and
  tools are sent too, whether or not they went through `UndoManager`.

**Values.** An op's value is any `VtValue`, of any of the types the Attribute Editor handles. The
plan is to let USD serialize it: the batch's fields are written to a scratch layer and sent in
that layer's file format. USDA text handles every type but is slow and large for a mesh's points;
crate is compact but is written to a file. *Measured* (§10.1): both carry every value the spike
tried exactly, and USDC is the format, six times faster to encode and thirty times faster to decode
than USDA on a million points. Tokens, token lists and path lists go in the batch's header
instead, since a children field in a scratch layer would name children it hasn't got.

## 5. Order and conflicts

The host numbers every batch. That order is the truth.

1. A peer applies its own batch at once and sends it to the host.
2. The host numbers it and sends it to everyone, the sender too, as the acknowledgement.
3. A peer that gets someone else's batch while its own are still unacknowledged applies it, then
   applies again those of its own ops that touch the same spec and field. The host will number
   them later, so they win.

Everyone ends with the same live layer. For one field of one spec, the last writer in the host's
order wins. An op on a spec that no longer exists is dropped. It is the rule of a shared map
([PEERS.md](PEERS.md) §7.2), with a spec's field in place of a key.

- **The unit is the field.** Anna moving a prim while Ben changes its color is no conflict. Both
  moving it is: the later one stays. A whole array, a mesh's points, is one field.
- **No locks and no merging of values** in the first version. Presence (§8) shows who is working
  on what, which avoids most collisions before they happen.
- **A remote batch is applied in one change block** with recording off: `Stage::changed` fires
  once, every panel refreshes as for a local edit, and nothing lands on the local undo stack.
- **A check.** When the room is quiet the host sends a hash of its live layer. A peer whose hash
  differs logs a warning and takes a fresh snapshot.

## 6. Undo

Each peer's undo stack holds its own steps only, as today (one `QUndoStack` per stage).

- **Undo is an edit.** Playing a step's inverses is recorded and sent like any batch.
- **Undoing under someone else's later change** puts the old value back anyway: last writer wins,
  as for any edit (§12).
- **An inverse for a spec someone else deleted** is dropped.

## 7. Joining late

The channel's snapshot is the live layer as USDA text, with the number of the last batch in it.
The joiner applies it and then every batch after that number. A reconnect too far behind does the
same.

## 8. Presence

Per-peer state, a few updates a second:

- **Selection:** a dot in each peer's color beside the prims they have selected in the Outliner,
  and an outline in that color in the viewport.
- **Camera:** where each peer is looking, drawn as a small frustum, and **Follow Anna**, which
  takes over the view and the current frame until the user moves.
- **Dragging:** which prim a peer has in a gizmo right now.

Each peer keeps its own selection, time, cameras, muted layers, load rules and session layer.
None of them is shared, only shown.

## 9. Ending

The host owns the files, so the host decides:

- **Merge** the live layer into the root layer, the layer that was the edit target, or a new
  layer file. Then it is an ordinary unsaved change, and Save writes it.
- **Keep** it as `<stage>.live.usda` beside the stage, to share again later.
- **Discard** it.

A member who leaves, or whose host left, can keep a copy of the live layer as a local sublayer.

## 10. Phases

They need [PEERS.md](PEERS.md) phases 1a and 1b, and nothing after them.

| Phase | Builds | Done when |
|---|---|---|
| **0. Spike** (done) | A recorder that replays one layer's ops onto another: `tests/stage_sync_spike_test.cpp` | It answers: whether replaying the recorded ops gives an identical layer; how to serialize an op's value, timed on a mesh of a million points. Answered in §10.1 |
| **A. Same files** (built) | The live layer, ops, order, the snapshot, undo, view-only members, merge | Two instances move different prims of one stage and each sees both; both move the same prim and end equal; undo on one side undoes only its own step; a third joins late and is equal; Merge and Save write the result |
| **B. Presence** (built) | Selections, cameras, Follow | A peer's selection and camera show in the other's Outliner and viewport |
| **C. Sending the stage** (built) | Packing and sending the files to a peer who doesn't have them | A peer with none of the files joins a shared stage |
| **Later** | Locks per prim | |

### 10.1 What the spike found

What the plan believed, and what `tests/stage_sync_spike_test.cpp` found (2026-11-21, Release
build, OpenUSD 26.08, Windows). Run it with `usdcc_tests stage_sync`.

The spike's `OpRecorder` is a layer state delegate, as `EditRecorder` is, that keeps each call
as an op instead of its inverse. A second layer that started equal gets the ops through its own
`OpRecorder`'s public calls (`SetField`, `CreateSpec`, `PushChild`, ...), in one change block. The
two layers are then compared spec by spec, field by field, and as USDA text.

1. **Replaying the ops gives an identical layer. Yes.** The edits were made through `UsdStage`
   as the app makes them: prims, a mesh, values of many types (matrix, half quaternion, string
   array, asset path, uniform token, doubles and floats that don't print short), time samples set,
   replaced and cleared, dictionary keys set and erased, layer metadata, references, payloads,
   inherits, API schemas, a shader connection, relationship targets, a variant set with prims
   inside a variant, rename and reparent (Sdf namespace edits), child reordering, a prim and a
   property deleted, a prim deactivated. Once on an empty layer (190 ops) and once on a scene the
   other side already had (10 ops). All equal: in memory, through USDA and through USDC.
2. **Whole-layer calls are recorded too.** `TransferContent` on a layer with a state delegate
   turns into ordinary ops (98 for a small scene), and `Clear` into the deletes (6). Both replay
   to an identical layer. So a script that replaces the live layer's content is shared like any
   edit.
3. **A new attribute's `custom` comes before its `typeName`.** Sdf sets the fields in that order
   when it creates an attribute, so when `custom` is recorded the spec has no type yet, and the
   scratch layer can't make an attribute to hold it. The `typeName` op always follows in the same
   batch, and the encoder takes the type from there. Phase A's recorder must do the same.
4. **USDA and USDC both round-trip every value exactly,** fallback values included (`custom = false`
   isn't lost), and so do 17-digit doubles. The header carries tokens, token lists and path lists
   itself; everything else goes in a scratch layer that mirrors the spec. A field set twice in one
   batch needs a second scratch layer (the new scene took 3).
5. **A million points** (`point3f[]`, 12 MB raw):

   | | Encode | Decode | Size |
   |---|---|---|---|
   | Raw copy (the floor) | 4 ms, out and back | | 11.4 MB |
   | USDC, through a temporary file | 80 ms | 18 ms | 11.4 MB |
   | USDA text | 500 ms | 600–690 ms | 33.2 MB |

   Recording the op costs nothing (0.1 ms: `VtArray` shares its data), and neither does applying
   it. **USDC for every batch**, then, not only the large ones: there is no size at which USDA
   wins, and the small batches above are a kilobyte either way. Reading USDC from memory needs an
   `ArAsset` of our own instead of the temporary file; that is phase A's work, not a question.

### 10.2 Phases A to C as built

The parts, all in `usdcc_usd_ui` under `share/`:

| Part | What it does |
|---|---|
| `layer_ops.h` | The ops, their encoding (a JSON header and USDC scratch layers), and `applyOps()`, which drops an op that no longer fits the layer |
| `undo_manager.h` | `UndoManager::observeLayer()`: the undo recorder also reports each change as an op. `changeUnrecorded()`: a peer's ops, applied without undo or reporting |
| `stage_sync.h` | `StageSync`: one live layer kept equal, knowing nothing of rooms (§4–§7) |
| `shared_stages.h` | `SharedStages`: sharing, opening, leaving, stopping, who may edit, presence and files, over a chat room |
| `shared_stages_section.h` | The Peers panel's section: Share Current Stage, Open, Leave, Stop Sharing, Who May Edit, Follow |
| `presence.h` | `PresenceHub`: the others' selections, views and frames, for the Outliner, the viewport and Follow |
| `live_layers.h` | Which stages are shared, for the rule that only prims made in the live layer move (§2) |

The channels of a share are `stage/<id>` (ordered), `stage/<id>/drag` (latest only),
`stage/<id>/ask` (direct, a member's requests to the host) and `stage/<id>/files` (transfers),
with the per-peer state `stage/<id>/presence` and the room's map `stages`, which only the host
writes.

**Where it differs from the plan:**

1. **Conflicts (§5) by rebuilding, not by applying again.** Each peer keeps a confirmed layer, the
   live layer in the host's order. When a peer's batch arrives while this peer's own are still on
   the way, the live layer is rebuilt as the confirmed one plus its own batches. That gives the
   same result as the plan's rule for one field, and holds for the specs too (created, deleted,
   moved), which re-applying field by field would not have. With nothing of its own on the way,
   a peer's batch is applied directly.
2. **A child list is sent as its changes.** Sdf records a parent's first child as a set of the
   whole list, which would drop a child another peer added meanwhile. Such a set goes out as the
   children added or taken out; only a reorder is sent as the list. Undoing one puts back the
   children it took out where they were, takes out those it added, and keeps a peer's since.
3. **A batch is numbered by its sender** as well as by the host, and a number seen before is
   ignored, so a peer can send its unconfirmed batches again after a snapshot without doubling them.
4. **The snapshot (§7) is USDC,** not USDA. One over 8 MB goes as a file (a transfer) the joiner
   asks for. A room sends a channel's snapshot only to members who join after the channel opened,
   so a member opening a stage shared earlier asks the host for one (`stage/<id>/ask`). A member
   that may only watch can still ask: requests have a channel of their own.
5. **A batch over 12 MB can't be sent** (the history and queues of a room aren't made for it):
   the change is taken back, and the user told. A drag's previews are sent only while under 1 MB.
6. **Presence's viewport outline is a box** around each prim a peer has selected, in its color,
   drawn as the collider overlay is, not Hydra's outline. Follow takes the peer's view into the
   orbit camera, and its frame; any move of the view stops it.
7. **Merge (§9) stitches** the live layer over the target layer (`UsdUtilsStitchLayers`), as one
   undo step, an unsaved change. Stop Sharing offers Merge into the root layer, Keep Beside the
   Stage (`<name>.live.usda`, offered again on the next share) and Discard.
8. **Phase C packs the stage, not a flattened copy:** `UsdUtilsLocalizeAsset` copies every layer
   and asset the root needs, with paths relative to it, once per share, under
   `<AppLocalData>/shared/<id>/`. Flattening would have lost the variants and layers that edits
   in the live layer refer to. The member opens the root from its inbox.
9. **A stage closed while shared** ends the share: the host's edits are kept beside the stage (or
   discarded, for a stage only in memory); a member's go with the stage.

**Not done yet:** the Layer Editor doesn't mark the live layer; whether a session
([SESSIONS.md](SESSIONS.md)) saved while sharing keeps the live layer's name in the session layer
isn't checked; peers' selections in Hydra's own outline; locks (§12 #2); two machines by hand
(§11).

## 11. Testing

*As built:* `tests/stage_sync_test.cpp` (usdcc_tests) joins three and four `StageSync`s by a pipe
that orders like a host and delivers at random: 30 seeds of random edits from every side, with a
late joiner, end equal; and last writer wins, a large snapshot sent as a file, undo on one side
only (and after a peer deleted the spec), a watcher refused, a drag's previews and its batch, the
check. `tests/shared_stages_test.cpp` (usdcc_ui_tests) runs three instances' `SharedStages` in a
real room on 127.0.0.1: share, open late, edits both ways, the rename rule, presence and Follow,
files sent to a member who hasn't them, a watcher, and Merge with undo.

The plan: `tests/stage_sync_test.cpp`: two stages in one process, their live layers joined by a pipe that
delays and reorders. Random edits from both sides must end in identical layers. Also the snapshot,
undo, an op for a deleted spec, and a view-only member's edit refused. By hand: two instances with
two test profiles on one machine, then two machines.

## 12. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Undo under someone else's later change: put the old value back, or skip? | Put it back, as for any edit. Change it if it surprises people in use |
| 2 | Locks ("Anna is editing this prim")? | Not in the first version. Presence shows it; a soft warning before a hard lock |
| 3 | May a member edit a layer other than the live layer? | No, in the first version. It is what keeps files safe and the sync small |
| 4 | Can a hub ([PEERS.md](PEERS.md) §9) host a shared stage, so it outlives any one artist? | Later. It would need the Python package to hold a live layer, which means OpenUSD on the hub |
