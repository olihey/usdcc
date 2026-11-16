// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "BLI_task.h"
#include "BLI_threads.h"
#include "CLG_log.h"
#include "MEM_guardedalloc.h"
#include <exception>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <future>
#include <queue>
#include <stdexcept>
#include <string>

namespace usdcc::blender_mesh {
inline std::string allocatorError;
// Shared by every backend entry point. Blender's allocator and thread globals
// must never be initialized concurrently, including diagnostic calls.
struct Runtime {
    Runtime() {
        MEM_use_guarded_allocator();
        MEM_set_error_callback([](const char* message) { allocatorError = message; });
        CLG_init();
        blender::BLI_threadapi_init();
        blender::BLI_task_scheduler_init();
    }
    ~Runtime() {
        blender::BLI_task_scheduler_exit();
        blender::BLI_threadapi_exit();
        CLG_exit();
    }
};
class Executor {
    std::mutex mutex, lifecycle;
    std::condition_variable ready;
    std::queue<std::function<void()>> queue;
    std::thread thread;
    bool stopping = false;
public:
    ~Executor() { stop(); }
    void run(std::function<void()> function) {
        // A shutdown cannot race a new submit. Holding lifecycle while waiting
        // also gives deterministic serialization to synchronous API callers.
        std::lock_guard life(lifecycle);
        if (!thread.joinable()) {
            stopping = false;
            thread = std::thread([this] {
                Runtime runtime;
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex);
                        ready.wait(lock, [&] { return stopping || !queue.empty(); });
                        if (stopping && queue.empty()) break;
                        job = std::move(queue.front()); queue.pop();
                    }
                    job(); // packaged_task captures operation exceptions
                }
            });
        }
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(function));
        auto future = task->get_future();
        { std::lock_guard lock(mutex); queue.emplace([task] { (*task)(); }); }
        ready.notify_one();
        future.get();
    }
    void stop() {
        std::lock_guard life(lifecycle);
        { std::lock_guard lock(mutex); stopping = true; }
        ready.notify_all();
        if (thread.joinable()) thread.join();
    }
};
inline Executor& executor() { static Executor value; return value; }
template<class Function> void onWorker(Function&& function) {
    executor().run([&] {
        allocatorError.clear();
        function();
        if (!allocatorError.empty()) throw std::runtime_error("Blender allocator error: " + allocatorError);
    });
}
}
