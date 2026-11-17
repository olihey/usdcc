# A Shared Stage (plan)

Several people editing one stage at once, each seeing the others' edits, selections and cameras.
One use of [PEERS.md](PEERS.md), and the largest.

**Status:** planned 2026-11-16 as part of PEERS.md, moved here 2026-11-17. Nothing is built yet.
"Checked" means looked at in this repository; everything else is an assumption until the spike
(§10).

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
crate is compact but is written to a file. Which, and where the size limit between them is, comes
from the spike.

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
| **0. Spike** | A recorder that replays one layer's ops onto another | It answers: whether replaying the recorded ops gives an identical layer; how to serialize an op's value, timed on a mesh of a million points |
| **A. Same files** | The live layer, ops, order, the snapshot, undo, view-only members, merge | Two instances move different prims of one stage and each sees both; both move the same prim and end equal; undo on one side undoes only its own step; a third joins late and is equal; Merge and Save write the result |
| **B. Presence** | Selections, cameras, Follow | A peer's selection and camera show in the other's Outliner and viewport |
| **C. Sending the stage** | Packing and sending the files to a peer who doesn't have them | A peer with none of the files joins a shared stage |
| **Later** | Locks per prim | |

## 11. Testing

`tests/stage_sync_test.cpp`: two stages in one process, their live layers joined by a pipe that
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
