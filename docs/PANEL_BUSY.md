# Busy Panels (plan)

Status: **plan**, nothing built. Planned 2026-11-12 against master `5644706` plus the uncommitted
USDA panel change ([USDA_VIEW.md](USDA_VIEW.md) §5.1). §8 lists the decisions still open.

A way for any view panel to say "I'm working on something": the panel dims, and over it a card
shows what it's doing, a progress bar and, where the work allows, Cancel. The rest of the app stays
usable. The USDA panel's conversion of a big binary layer is the first user.

## 1. Does a progress dialog make sense for the USDA conversion?

**A dialog, no. A busy panel with the conversion off the UI thread, yes.**

- Today the conversion (`SdfLayer::ExportToString()`) runs on the UI thread. While it runs nothing
  paints, so a progress bar of any kind would sit frozen, and the whole app waits, as the lion file
  showed: 77 s for 1.95 GB of usdc.
- A modal progress dialog would block the whole app for something only one panel needs. The stage
  opener has one (`StageOpener`, "Opening lion_statue_hq.usd..."). That makes sense there, because
  opening a stage concerns every panel, but it doesn't here.
- Moved to a worker thread (§5), the conversion leaves the app responsive. Then the panel itself is
  the right place to show progress: the user can keep working in the viewport and Outliner while the
  USDA tab finishes.
- USD's writer reports no progress and can't be stopped (§5.3), so the progress is an estimate and
  Cancel throws the result away rather than stopping the work.

## 2. What exists

| Where | How it says it's busy |
|---|---|
| `StageOpener` | An app-modal `QProgressDialog` after a delay, indeterminate, then "Preparing viewport..." until the viewport's first frame |
| `UsdaView::refresh()` | A wait cursor and, for the flattened stage, "Flattening..." in the status line, painted once before blocking |
| `UsdaView` (§5.1) | The Convert bar: asks first, then converts on the UI thread |
| `RenderViewPanel` | Its own status line ("Rendering", "Done") |
| `UsdaPreviewDialog` | "a stage is coming" in Show, while a worker loads the stage |

Nothing is shared, and none of them dims the panel or shows progress.

## 3. The API: `ViewPanel::beginBusy()`

On `usdcc::ui::ViewPanel`, so every panel has it, C++ and Python (§6):

```cpp
// While the returned handle lives, the panel is busy: dimmed, with a card showing
// `text`, a progress bar and, if `onCancel` is given, Cancel. Several handles may live
// at once (two pieces of work): the newest one's card shows.
std::shared_ptr<ViewPanel::Busy> beginBusy(const QString& text, BusyOptions options = {});
bool isBusy() const;
Q_SIGNAL void busyChanged(bool busy);

struct BusyOptions {
    std::function<void()> onCancel;  // none: no Cancel button
    bool paintNow = false;           // see §3.2
    int showAfterMs = 300;           // quick work never shows the overlay
};

class ViewPanel::Busy {
public:
    // Any thread: the panel picks the latest value up on the UI thread, at most ~30 times
    // a second.
    void setText(const QString& text);
    void setDetail(const QString& detail);   // a second, smaller line: "1.2 GB written"
    void setProgress(double fraction);       // 0-1; negative: indeterminate (the default)
    bool isCancelled() const;                // Cancel was clicked (or the panel closed)
    void finish();                           // the same as dropping the last reference
};
```

### 3.1 How it looks and behaves

- **Dim:** the whole panel, toolbar included, under a semi-transparent black (about 45%, as the
  HUD's plates), so it's clear nothing in it can be used until the work is done.
- **Card:** centred, the panel's surface colour, rounded, with the text, a progress bar (determinate
  or the indeterminate stripe), the detail line, the time so far once past 2 s ("0:34"), and Cancel.
  It shrinks to fit a narrow panel; under about 160×100 px only the dim and a spinner show.
- **Delay and fade:** it appears `showAfterMs` after `beginBusy()` (300 ms), with a 120 ms fade, so
  work that ends quickly doesn't flash. It goes as soon as the last handle is dropped.
- **Input:** the overlay takes the mouse, wheel and keys over the panel's content. Esc cancels when
  Cancel is shown. The panel's tab stays usable: it can be dragged, switched away from, floated and
  closed.
- **Closing a busy panel:** calls every handle's `onCancel` first. The work must not touch the panel
  afterwards: it holds a `QPointer` or the `Busy` handle, never the panel directly.
- **Hidden panels:** a panel behind another tab is busy all the same. The overlay is there when it
  comes to the front.

### 3.2 Work on the UI thread

The overlay can only animate while the UI thread is free, so the work should run on a worker thread.
Some work can't, though: USD can't flatten a stage while it may be edited, and Hydra populates on the
thread that renders. For those, `paintNow` shows the overlay at once, painted synchronously
(`repaint()`) before `beginBusy()` returns. The card then sits still, with an indeterminate bar,
until the work ends: still better than a frozen panel that looks idle.

### 3.3 Threads

`Busy`'s setters post to the UI thread (`QMetaObject::invokeMethod`, queued) and coalesce, so a
worker can call `setProgress()` in a tight loop. `isCancelled()` is an atomic. The handle outlives
the panel safely: once the panel is gone, its setters do nothing.

## 4. Panels with native windows

A child widget can't cover a native window: on Windows a native child draws over any plain sibling
widget ([NOTIFICATIONS.md](NOTIFICATIONS.md) §2). Three panels have one: the viewport (Hydra's
`QWindow`), the RTX Render view (Vulkan) and the Material Editor's preview.

**Proposed: snapshot and step aside.** While the overlay shows, each native window in the panel is
hidden and replaced by a still picture of its last frame, dimmed under the card. The overlay finds
them itself (the `createWindowContainer()` widgets in the panel), takes the picture with
`QScreen::grabWindow()` of the container, and shows them again when the panel stops being busy. A
panel can do better with `virtual QImage busySnapshot()`: the viewport returns
`HydraViewportWindow::grabFrame()`, which works even when the window is covered. Hiding the window
only stops it drawing; its engine stays (it's freed only when the surface goes).

*Alternative:* the overlay as its own frameless, translucent tool window above the panel, as the
toasts are. It covers anything, but it has to follow the panel through docking, floating, tab
switches and minimising, which the toasts only do for the main window. More can go wrong.

## 5. The USDA conversion in the background

### 5.1 What runs where

| Source | UI thread | Worker |
|---|---|---|
| A layer whose file is as saved (the lion) | nothing | `SdfLayer::OpenAsAnonymous(file)`, its own copy, then `Export()` to a temporary `.usda` |
| A layer with unsaved changes, or an anonymous one | a copy into a new anonymous layer (`TransferContent()`), under a `paintNow` overlay | `Export()` of the copy |
| Flattened layer stack | `UsdUtilsFlattenLayerStack()` (`paintNow`) | `Export()` of the flattened layer |
| Flattened stage | `UsdStage::Flatten()` (`paintNow`), as today | `Export()` of the result |
| A text file | read as it is (fast, no overlay) | — |

The worker never touches a layer the app can edit: each conversion has its own copy, so edits on the
UI thread meanwhile are safe. The text then shows "Out of date" if the stage changed while it ran.

### 5.2 Progress

The worker exports to a temporary file rather than a string. Its size as it grows is the progress.
That also means the panel never holds 2 GB of text, only what it shows (`kShowLimitBytes`); Show All
reads the rest from the same file.

- **Detail:** "1.2 GB of text · 0:34".
- **Bar:** text written against an estimate of the total. The lion's 1.95 GB of usdc made 2.15 GB of
  text (×1.1), but the ratio depends on the data. So the bar is the estimate, held at 95% until
  the export ends (§8, question 3).

### 5.3 Cancel and quitting

- USD can't stop an export. Cancel drops the result: the panel goes back to the Convert bar at once,
  and the worker finishes on its own, then deletes its temporary file and its copy of the layer.
  Converting again doesn't wait for it.
- **Quitting** while a worker runs: the worker can't be stopped, and a thread still inside USD as
  the app exits can crash at shutdown. So quitting waits for running conversions, with a note in
  the main window ("Finishing a USDA conversion..."), and after 10 s exits without waiting
  (`quick_exit`, skipping static destructors; §8, question 4).
- Temporary files go to the app's temp folder (`usdcc-usda-*.usda`). Leftovers from a crash are
  removed at the next start.

### 5.4 The Convert bar stays

With the app responsive, the bar is no longer about freezing. It's about the cost: a minute of CPU,
2 GB of temporary disk and up to 1 GB of memory for text that's mostly never read. Its message
changes to say so ("Converting it to text takes about a minute and 2 GB of disk; the app stays
usable meanwhile."), and Convert starts the background conversion.

## 6. Python

`usdcc.ui.ViewPanel` gets the same, as a context manager:

```python
with self.busy("Baking lights", cancellable=True) as busy:
    for i, item in enumerate(items):
        if busy.cancelled:
            break
        bake(item)
        busy.progress = (i + 1) / len(items)
```

On the UI thread, as Python panels usually work, the overlay can't animate, so `busy()` there uses
`paintNow` and `progress` repaints the card itself (`repaint()`, at most ~10 times a second). From a
Python thread it behaves as in C++.

## 7. Phases

| Phase | Contents | Verified |
|---|---|---|
| **0. Spike** | The overlay over a viewport panel: hiding the Hydra window, the snapshot, showing it again; the same over the RTX view | In the app: no flicker or black frame, the viewport draws again at once afterwards, its engine kept; RTX view the same |
| **1. API and overlay** | `beginBusy()`, `Busy`, the overlay widget (dim, card, delay, fade, input, Esc, cancel on close), thread-safe updates | Tests: shows after the delay, not before; takes clicks and keys; progress and text from a worker thread; two handles; closing cancels; a handle outliving its panel |
| **2. Native windows** | Snapshot and step aside; `busySnapshot()` for the viewport | Tests: the containers hidden and back; in the app: viewport, Material Editor preview, RTX |
| **3. USDA in the background** | §5: worker export to a temporary file, progress, Cancel, Show All from the file, the copies for unsaved layers and flattened views, quitting, the new bar text | Tests: a generated usdc converts in the background with progress, edits meanwhile mark it out of date, Cancel, unsaved layer, temp file cleanup; in the app: the lion file, using the viewport while it converts |
| **4. Python** | `ViewPanel.busy()` | Python tests: UI-thread and thread use, cancel |

Phases 1 and 2 are the general mechanism; 3 is the first user. Later users, not in this plan: the
RTX Render view while its SDK and GPU load, the Dependencies panel's scan, the Material Editor
opening a big network, and the viewport's "Preparing viewport..." (Hydra's first population),
which would move from the app-modal dialog to the viewport alone.

## 8. Open decisions

1. **Native windows: snapshot and step aside, or a tool window?** Proposed: snapshot (§4).
2. **Does the dim cover the toolbar?** Proposed: yes, the whole panel as asked. The tab stays usable,
   so the user can always switch away or close it.
3. **The USDA bar: an estimate, or indeterminate with the bytes written?** Proposed: the estimate,
   held at 95% until done, with the bytes written as the detail.
4. **Quitting during a conversion:** wait up to 10 s, then exit without static destructors? Or ask
   ("A USDA conversion is still running") with Wait / Quit Anyway?
5. **Keep the Convert bar** now that the app doesn't freeze? Proposed: yes, for the disk, memory
   and CPU it costs (§5.4), with the new message.
