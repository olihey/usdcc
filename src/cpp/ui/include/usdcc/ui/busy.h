#pragma once

#include "usdcc/core/export.h"

#include <QString>

#include <functional>
#include <memory>

namespace usdcc::ui {

// How ViewPanel::beginBusy() shows (docs/PANEL_BUSY.md §3).
struct BusyOptions {
    // Called on the UI thread when the user cancels (Cancel, Esc) or the panel closes.
    // None: no Cancel button.
    std::function<void()> onCancel;
    // For work on the UI thread, which leaves the overlay no chance to appear on its own:
    // it shows at once, painted before beginBusy() returns, and setProgress()/setText() from
    // the UI thread repaint it (at most about ten times a second). See §3.2.
    bool paintNow = false;
    // Work that ends sooner never shows the overlay.
    int showAfterMs = 300;
};

// While a handle from ViewPanel::beginBusy() lives, the panel is busy (docs/PANEL_BUSY.md §3).
// Its setters may be called from any thread: the panel shows the latest values on the UI
// thread, about 30 times a second. A handle outliving its panel is harmless: it does nothing.
class USDCC_UI_API Busy {
public:
    struct State;
    explicit Busy(std::shared_ptr<State> state);
    ~Busy();  // finish()
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;

    void setText(const QString& text);
    // A second, smaller line: "1.2 GB written".
    void setDetail(const QString& detail);
    // 0 to 1; negative (the default): an indeterminate bar.
    void setProgress(double fraction);
    // Cancel was clicked, Esc pressed, or the panel closed.
    bool isCancelled() const;
    // The work is done: the same as dropping the last reference.
    void finish();

private:
    std::shared_ptr<State> m_state;
};

}  // namespace usdcc::ui
