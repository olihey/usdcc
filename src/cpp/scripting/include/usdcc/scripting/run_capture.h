#pragma once

#include "usdcc/core/log.h"

#include <functional>
#include <memory>

namespace usdcc::scripting {

// Whether `record`, logged while a script runs or a plugin reloads, is part of its
// output: not the run's own bookkeeping ("Ran x.py in 3 ms"), and not what the plugin
// manager says of a failure, which the traceback tells.
bool isRunOutput(const usdcc::core::LogRecord& record);

// Hands over a run's output as it is logged, for as long as it exists. Identical records
// in a row, which the log collapses into one, come once each. The handler is called on
// whichever thread logged, with the log's lock held: it must be quick and must not log.
class RunCapture {
public:
    using Handler = std::function<void(const usdcc::core::LogRecord&)>;
    explicit RunCapture(Handler handler);
    ~RunCapture();
    RunCapture(const RunCapture&) = delete;
    RunCapture& operator=(const RunCapture&) = delete;

private:
    std::shared_ptr<usdcc::core::LogSink> m_sink;
};

}  // namespace usdcc::scripting
