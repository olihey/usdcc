#include "usdcc/scripting/run_capture.h"

#include <utility>

namespace usdcc::scripting {

namespace {
struct Sink : usdcc::core::LogSink {
    RunCapture::Handler handler;
    void onRecord(const usdcc::core::LogRecord& record) override {
        if (isRunOutput(record)) handler(record);
    }
    void onRepeat(const usdcc::core::LogRecord& record) override { onRecord(record); }
};
}  // namespace

bool isRunOutput(const usdcc::core::LogRecord& record) {
    if (record.source == "usdcc" && record.level < usdcc::core::LogLevel::Warning) return false;
    return record.category != "usdcc.plugins";
}

RunCapture::RunCapture(Handler handler) {
    auto sink = std::make_shared<Sink>();
    sink->handler = std::move(handler);
    m_sink = std::move(sink);
    usdcc::core::LogHub::instance().addSink(m_sink, false);
}

RunCapture::~RunCapture() { usdcc::core::LogHub::instance().removeSink(m_sink.get()); }

}  // namespace usdcc::scripting
