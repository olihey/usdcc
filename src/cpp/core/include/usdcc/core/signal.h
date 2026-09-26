#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace usdcc::core {

// A minimal, Qt-free multicast delegate for non-UI classes that need to
// notify listeners without pulling in QObject/moc (see docs/PLAN.md's "Qt
// only in UI classes" rule) — e.g. usdcc::usd::StageManager, whose Qt-facing
// UI listeners (ViewportViewPanel, OutlinerViewPanel, AttributesViewPanel)
// connect to it via Signal::connect() instead of Qt's connect().
//
// connect() returns a move-only Connection that disconnects its callback
// when destroyed (or via Connection::disconnect()) — the same safety
// QObject::connect()'s automatic disconnect-on-destroy provides, without
// requiring either side to derive from QObject. Safe in either destruction
// order: if the Signal itself is destroyed first, a still-alive
// Connection's disconnect() becomes a harmless no-op (via the weak_ptr
// below) instead of dereferencing a dangling pointer.
//
// Not thread-safe: callbacks fire synchronously, on whichever thread calls
// operator().
//
// Deliberately doesn't call its internal storage "slots": Qt's <QObject>
// #defines `slots` (and `signals`/`emit`) as bare macros project-wide
// (QT_NO_KEYWORDS is never set here, since UI code relies on that syntax),
// so any Qt-including translation unit that reaches this header afterward
// would silently mangle a member actually named that.
template <typename... Args>
class Signal {
public:
    using Callback = std::function<void(Args...)>;

private:
    struct Impl {
        std::vector<std::pair<std::size_t, Callback>> callbacks;
        std::size_t nextId = 0;
    };

public:
    class Connection {
    public:
        Connection() = default;
        Connection(Connection&&) noexcept = default;
        Connection& operator=(Connection&& other) noexcept {
            if (this != &other) {
                disconnect();
                m_impl = std::move(other.m_impl);
                m_id = other.m_id;
            }
            return *this;
        }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        ~Connection() { disconnect(); }

        void disconnect() {
            if (auto impl = m_impl.lock()) {
                auto& callbacks = impl->callbacks;
                callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                                                [this](const auto& entry) { return entry.first == m_id; }),
                                 callbacks.end());
            }
            m_impl.reset();
        }

    private:
        friend class Signal;
        Connection(std::weak_ptr<Impl> impl, std::size_t id) : m_impl(std::move(impl)), m_id(id) {}
        std::weak_ptr<Impl> m_impl;
        std::size_t m_id = 0;
    };

    Signal() : m_impl(std::make_shared<Impl>()) {}
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    Connection connect(Callback callback) {
        const std::size_t id = m_impl->nextId++;
        m_impl->callbacks.emplace_back(id, std::move(callback));
        return Connection(m_impl, id);
    }

    void operator()(Args... args) const {
        // Copy the callback list first: a callback may disconnect (or
        // connect another) while this signal is firing.
        auto callbacks = m_impl->callbacks;
        for (auto& entry : callbacks) {
            entry.second(args...);
        }
    }

private:
    std::shared_ptr<Impl> m_impl;
};

}  // namespace usdcc::core
