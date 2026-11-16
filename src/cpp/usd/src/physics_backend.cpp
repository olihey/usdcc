#include "usdcc/usd/physics_backend.h"

#include <algorithm>

namespace usdcc::usd::physics {

PhysicsBackends& PhysicsBackends::instance() {
    static PhysicsBackends backends;
    return backends;
}

void PhysicsBackends::add(std::shared_ptr<PhysicsBackend> backend) {
    if (!backend) return;
    const std::string id = backend->id();
    const auto same = std::find_if(m_backends.begin(), m_backends.end(), [&id](const auto& b) { return b->id() == id; });
    if (same != m_backends.end()) {
        *same = std::move(backend);
    } else {
        m_backends.push_back(std::move(backend));
    }
    changed();
}

void PhysicsBackends::remove(const std::string& id) {
    const auto end = std::remove_if(m_backends.begin(), m_backends.end(), [&id](const auto& b) { return b->id() == id; });
    if (end == m_backends.end()) return;
    m_backends.erase(end, m_backends.end());
    changed();
}

std::shared_ptr<PhysicsBackend> PhysicsBackends::current() const {
    return m_backends.empty() ? nullptr : m_backends.front();
}

}  // namespace usdcc::usd::physics
