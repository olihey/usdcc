#pragma once

#include "usdcc/core/log.h"

#include <QString>
#include <QtGlobal>

namespace usdcc::ui {

// Routes Qt's own messages — qDebug/qInfo/qWarning/qCritical/qFatal and their
// qC* category variants, from Qt, ADS, QWindowKit and our code — into the
// usdcc log (docs/LOGGING.md §3.2), as records with source "qt" and the
// message's logging category ("default" for plain qWarning()). File, line and
// function are only known when Qt records them (Debug builds).
//
// Installed first thing in main(), before QApplication, so Qt's start-up
// warnings are caught. The previous handler is not called: ConsoleLogSink
// already prints everything to stderr and the debugger in Debug builds. Qt
// aborts by itself after a qFatal(); the hub flushes the log first.
class QtMessageBridge {
public:
    QtMessageBridge();   // installs the handler
    ~QtMessageBridge();  // restores the previous one
    QtMessageBridge(const QtMessageBridge&) = delete;
    QtMessageBridge& operator=(const QtMessageBridge&) = delete;

    // The record for one message (exposed for tests).
    static usdcc::core::LogRecord toRecord(QtMsgType type, const QMessageLogContext& context, const QString& message);

private:
    QtMessageHandler m_previous = nullptr;
};

}  // namespace usdcc::ui
