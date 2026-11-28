#pragma once

// A mouse button with the modifier keys held: what a MouseBinding setting stores
// (docs/SETTINGS.md §2), and what a navigation scheme binds orbit, pan and zoom to
// (docs/NAVIGATION.md §3). Qt-free; usdcc/ui/navigation.h matches it to Qt's events.

#include "usdcc/core/export.h"

#include <optional>
#include <string>
#include <string_view>

namespace usdcc::core {

enum class MouseButton { None, Left, Middle, Right };

struct MouseBinding {
    MouseButton button = MouseButton::None;  // None: bound to nothing
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;

    bool isSet() const { return button != MouseButton::None; }
    bool hasModifiers() const { return ctrl || shift || alt || meta; }
    bool operator==(const MouseBinding&) const = default;
};

// "Alt+Left", "Ctrl+Shift+Middle", "Right"; "" for a binding to nothing. The modifiers
// in the order QKeySequence's portable text has them: Ctrl, Shift, Alt, Meta.
USDCC_CORE_API std::string mouseBindingText(const MouseBinding& binding);
// The binding `text` names, whatever the case and order of its parts; "" is the binding
// to nothing. Nothing for text that isn't one: an unknown part, no button, modifiers alone.
USDCC_CORE_API std::optional<MouseBinding> parseMouseBinding(std::string_view text);

}  // namespace usdcc::core
