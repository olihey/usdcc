#include "usdcc/core/mouse_binding.h"

#include <algorithm>
#include <cctype>

namespace usdcc::core {

namespace {

std::string lowered(std::string_view text) {
    std::string result;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

}  // namespace

std::string mouseBindingText(const MouseBinding& binding) {
    if (!binding.isSet()) return {};
    std::string text;
    if (binding.ctrl) text += "Ctrl+";
    if (binding.shift) text += "Shift+";
    if (binding.alt) text += "Alt+";
    if (binding.meta) text += "Meta+";
    switch (binding.button) {
    case MouseButton::Left: text += "Left"; break;
    case MouseButton::Middle: text += "Middle"; break;
    case MouseButton::Right: text += "Right"; break;
    case MouseButton::None: break;
    }
    return text;
}

std::optional<MouseBinding> parseMouseBinding(std::string_view text) {
    const std::string all = lowered(text);
    MouseBinding binding;
    if (all.empty()) return binding;
    std::size_t from = 0;
    while (from <= all.size()) {
        const std::size_t plus = std::min(all.find('+', from), all.size());
        const std::string_view part = std::string_view(all).substr(from, plus - from);
        from = plus + 1;
        if (part == "ctrl") {
            binding.ctrl = true;
        } else if (part == "shift") {
            binding.shift = true;
        } else if (part == "alt") {
            binding.alt = true;
        } else if (part == "meta") {
            binding.meta = true;
        } else if (binding.isSet()) {
            return std::nullopt;  // two buttons, or something unknown after one
        } else if (part == "left") {
            binding.button = MouseButton::Left;
        } else if (part == "middle") {
            binding.button = MouseButton::Middle;
        } else if (part == "right") {
            binding.button = MouseButton::Right;
        } else {
            return std::nullopt;
        }
    }
    if (!binding.isSet()) return std::nullopt;  // modifiers alone
    return binding;
}

}  // namespace usdcc::core
