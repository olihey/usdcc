#pragma once

#include <QStringView>

#include <cstdint>
#include <string_view>
#include <vector>

namespace usdcc::usd::usda {

// USDA syntax for the USDA view's highlighter (docs/USDA_VIEW.md §3).
enum class Token { Keyword, Type, String, AssetPath, Path, Number, Key, Comment };

struct Span {
    int start;
    int length;
    Token kind;
};

// A line's only context: whether it starts inside a multi-line string, and
// which kind ("""...""" or '''...''').
enum State : std::uint8_t { Outside = 0, InTripleDouble = 1, InTripleSingle = 2 };

// The coloured spans of one line (no newline), starting in `*state`, which
// it leaves as the line ends.
std::vector<Span> tokenize(QStringView line, State* state);

// The state each line starts in, for a whole USDA text (UTF-8, '\n' line
// ends): one byte scan, far cheaper than tokenizing, so the view can colour
// any line on its own when it comes into view. Line i of the result is line
// i of the text.
std::vector<std::uint8_t> lineStates(std::string_view text);

}  // namespace usdcc::usd::usda
