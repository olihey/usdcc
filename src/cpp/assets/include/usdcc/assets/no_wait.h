#pragma once

// Resolving without waiting for a server (docs/REMOTE_ASSETS.md §10): for a UI thread, which
// shows what is known and the rest when it comes. Below Ar, so that it covers what resolves
// on its own: SdfLayer::Find(), and reading an attribute of asset type.

#include "usdcc/assets/lookup.h"

#include <cstddef>
#include <functional>
#include <string>

namespace usdcc::assets {

// While one of these is on a thread's stack, that thread's resolving asks no server: it has
// what servers last answered, to anyone in this process, and a path nobody asked about yet
// doesn't resolve. Such a path, and one answered a while ago, is asked about on another
// thread; the listener is told when that changes the answer.
//
// For reading only. A path that doesn't resolve here may be there: unanswered() says so.
class NoWait {
public:
    NoWait();
    ~NoWait();
    NoWait(const NoWait&) = delete;
    NoWait& operator=(const NoWait&) = delete;

    static bool active();
    // How often this thread had no answer. One that goes up during a resolve says that its
    // "not found" only means "not known yet".
    static std::size_t unanswered();
};

// Called from any thread when an answer came that something was waiting for, or that differs
// from the one before. One for the process; null for none.
void setAnswerListener(std::function<void()> listener);

// Drops what hasn't been asked yet, and waits for what is being asked, for at most two
// seconds: before the process ends.
void stopAsking();

// Of findInStack(): what every search found, and what a NoWait search gets.
void rememberAnswer(const Stack& stack, const std::string& path, const Found& found);
Found answerWithoutWaiting(const Stack& stack, const std::string& path);

// The warning for a file that wasn't found because a server couldn't be asked; none for one
// that is missing.
void warnNotAnswered(const Found& found, const std::string& display);

}  // namespace usdcc::assets
