#pragma once

#include <QStringView>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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

// Where each prim is defined in a USDA text (docs/USDA_VIEW.md §6): its path
// ("/World/Chair") to the line of its def, over or class, 0-based. Prims in
// a variant ("/World{look=red}Chair") are listed by their path without the
// variant selection, the first one written winning. One pass, following
// braces outside strings and comments: a prim's body is the { after its
// header (and its metadata in parentheses); any other { is a dictionary,
// time samples, a variant set or a variant.
std::unordered_map<std::string, int> primLines(std::string_view text);

}  // namespace usdcc::usd::usda
