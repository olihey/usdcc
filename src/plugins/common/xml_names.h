#pragma once

// XML as the protocols' servers send it (WebDAV's multistatus, S3's listings and
// errors): elements found by the name after their namespace prefix, which servers pick
// themselves (D:, d:, lp1:, none). Header-only, for the protocol plugins, which link
// pugixml; usdcc_assets doesn't.

#include <pugixml.hpp>

#include <cstring>

namespace usdcc::assets::xml {

inline const char* localName(const pugi::xml_node& node) {
    const char* name = node.name();
    const char* colon = std::strchr(name, ':');
    return colon ? colon + 1 : name;
}

inline pugi::xml_node childNamed(const pugi::xml_node& parent, const char* name) {
    for (const pugi::xml_node& child : parent.children()) {
        if (std::strcmp(localName(child), name) == 0) return child;
    }
    return pugi::xml_node();
}

// The text of the child of that name; empty when there's none.
inline const char* textOf(const pugi::xml_node& parent, const char* name) { return childNamed(parent, name).text().get(); }

}  // namespace usdcc::assets::xml
