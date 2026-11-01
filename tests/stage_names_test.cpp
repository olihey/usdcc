// stage_names: file names only, extended by parent folders just far enough to
// tell same-named files apart.
#include "test_harness.h"

#include "usdcc/usd/stage_names.h"

#include <string>
#include <vector>

using usdcc::usd::shortLayerNames;

namespace {
std::string joined(const std::vector<std::string>& names) {
    std::string s;
    for (const std::string& name : names) s += "[" + name + "]";
    return s;
}
}  // namespace

USDCC_TEST(stage_names) {
    using Names = std::vector<std::string>;

    CHECK(shortLayerNames({}).empty(), "no stages, no names");

    Names names = shortLayerNames({"C:/p/shots/a.usda", "C:/q/b.usda"});
    CHECK(names == Names({"a.usda", "b.usda"}), "unique file names stay short: %s", joined(names).c_str());

    names = shortLayerNames({"C:/p/shots/a.usda", "C:/p/assets/a.usda", "C:/q/b.usda"});
    CHECK(names == Names({"shots/a.usda", "assets/a.usda", "b.usda"}), "one folder tells them apart: %s",
          joined(names).c_str());

    // Only the names that still collide grow: the first differs one level up,
    // the other two only three levels up.
    names = shortLayerNames({"/x/one/a.usda", "/y/two/shot/a.usda", "/z/two/shot/a.usda"});
    CHECK(names == Names({"one/a.usda", "y/two/shot/a.usda", "z/two/shot/a.usda"}),
          "each name extends on its own: %s", joined(names).c_str());

    // Backslashes count as separators; the result always uses '/'.
    names = shortLayerNames({"C:\\p\\shots\\a.usda", "C:\\p\\assets\\a.usda"});
    CHECK(names == Names({"shots/a.usda", "assets/a.usda"}), "Windows paths: %s", joined(names).c_str());

    // A name that runs out of folders stops; the other keeps going.
    names = shortLayerNames({"shots/a.usda", "/p/shots/a.usda"});
    CHECK(names == Names({"shots/a.usda", "p/shots/a.usda"}), "one is a suffix of the other: %s",
          joined(names).c_str());

    // Anonymous layers show their tag.
    names = shortLayerNames({"anon:0000012345678:tmp.usda", "C:/p/b.usda"});
    CHECK(names == Names({"tmp.usda", "b.usda"}), "anonymous layer: %s", joined(names).c_str());
}
