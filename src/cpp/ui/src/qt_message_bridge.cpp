#include "usdcc/ui/qt_message_bridge.h"

#include <utility>

namespace usdcc::ui {

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
LogLevel levelFor(QtMsgType type) {
    switch (type) {
        case QtDebugMsg: return LogLevel::Debug;
        case QtInfoMsg: return LogLevel::Info;
        case QtWarningMsg: return LogLevel::Warning;
        case QtCriticalMsg: return LogLevel::Error;
        case QtFatalMsg: return LogLevel::Fatal;
    }
    return LogLevel::Warning;
}

void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    LogHub::instance().post(QtMessageBridge::toRecord(type, context, message));
}
}  // namespace

QtMessageBridge::QtMessageBridge() : m_previous(qInstallMessageHandler(handleMessage)) {}

QtMessageBridge::~QtMessageBridge() { qInstallMessageHandler(m_previous); }

LogRecord QtMessageBridge::toRecord(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    LogRecord record;
    record.level = levelFor(type);
    record.source = "qt";
    record.category = context.category ? context.category : "default";
    record.message = message.toStdString();
    if (context.file) {
        record.file = context.file;
        record.line = context.line;
    }
    if (context.function) record.function = context.function;
    return record;
}

}  // namespace usdcc::ui
