#include "usdcc/usd/render/final_renderer.h"

#include <algorithm>

namespace usdcc::render {

FinalRenderers& FinalRenderers::instance() {
    static FinalRenderers renderers;
    return renderers;
}

void FinalRenderers::add(std::shared_ptr<FinalRenderer> renderer) {
    if (!renderer) return;
    const std::string id = renderer->id();
    const auto same = std::find_if(m_renderers.begin(), m_renderers.end(), [&id](const auto& r) { return r->id() == id; });
    if (same != m_renderers.end()) {
        *same = std::move(renderer);
    } else {
        m_renderers.push_back(std::move(renderer));
    }
    changed();
}

void FinalRenderers::remove(const std::string& id) {
    const auto end = std::remove_if(m_renderers.begin(), m_renderers.end(), [&id](const auto& r) { return r->id() == id; });
    if (end == m_renderers.end()) return;
    m_renderers.erase(end, m_renderers.end());
    changed();
}

std::shared_ptr<FinalRenderer> FinalRenderers::find(const std::string& id) const {
    const auto found = std::find_if(m_renderers.begin(), m_renderers.end(), [&id](const auto& r) { return r->id() == id; });
    return found != m_renderers.end() ? *found : nullptr;
}

std::string settingAttribute(const std::string& ns, const std::string& key) {
    if (ns.empty() || key.rfind(ns + ":", 0) == 0) return key;
    return ns + ":" + key;
}

}  // namespace usdcc::render
