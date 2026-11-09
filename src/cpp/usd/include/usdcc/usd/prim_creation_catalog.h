#pragma once

#include <map>
#include <string>
#include <vector>

namespace usdcc::usd {

// What the Create menu offers, and how it's grouped (docs/CREATE_MENU.md §4). USD knows
// which prim types exist but not how to present them, so that's ours: a runtime
// registry, from C++ and from Python (usdcc.usd), that the app fills with the same
// calls a plugin uses.
//
// Entries are by family: "Capsule" sets, hides or removes both Capsule and Capsule_1.
// A family with several versions has a menu line for each, next to each other: the
// original by the family's name, the newer ones by their own (Capsule_1), since not
// every renderer knows the newer ones.
class PrimCreationCatalog {
public:
    // Directly in the menu rather than in a submenu.
    static constexpr const char* kTopLevel = "";
    // Where a type with no entry goes, so that everything can always be created.
    static constexpr const char* kPlugins = "Plugins";
    static constexpr int kLastInGroup = 1000;

    struct Entry {
        std::string category = kPlugins;
        int group = 0;               // groups are separated by a line
        int order = kLastInGroup;    // within the group; then by name
        bool hidden = false;
    };

    // One line of the menu.
    struct Item {
        std::string label;  // "Capsule" for the family's first version, "Capsule_1" for a later one
        std::string type;   // what's created: "Capsule", "Capsule_1"
        std::string documentation;
    };
    struct Category {
        std::string name;
        std::vector<std::vector<Item>> groups;  // none of them empty
    };

    // The app's own, which starts with the built-in entries.
    static PrimCreationCatalog& instance();
    PrimCreationCatalog();

    // Adds an entry, or replaces the one for that type, so running a script again
    // changes nothing and a built-in one can be overridden. Either spelling of a
    // versioned type names its family. A type USD doesn't know is kept, and left out of
    // the menu until its plugin has loaded.
    void set(const std::string& type, const Entry& entry);
    // Leaves it out of the menu.
    void hide(const std::string& type);
    // Forgets the entry, so the type goes to Plugins. False if there was none.
    bool remove(const std::string& type);
    // Back to only the built-in entries.
    void reset();

    // Every concrete type USD knows now, in the menu's order: the top level, the
    // built-in categories, others by name, then Plugins. Empty categories are left out.
    std::vector<Category> categories() const;

private:
    std::map<std::string, Entry> m_entries;
};

}  // namespace usdcc::usd
