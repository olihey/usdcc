#pragma once

#include "usdcc/usd/prim_context_menu.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFileOpenEvent>
#include <QObject>
#include <QStringList>

#include <utility>

namespace usdcc::app {

// Files the system asks the app to open: on macOS a double-click in Finder, or a drop on the
// Dock icon, is an event and not an argument (docs/MACOS.md). One that comes before the app
// can open stages waits for open().
class FileOpenEvents : public QObject {
public:
    explicit FileOpenEvents(QCoreApplication* app) : QObject(app) { app->installEventFilter(this); }

    // From now on files are opened as they come; first those that have been waiting.
    void open() {
        m_ready = true;
        const QStringList waiting = std::exchange(m_waiting, {});
        for (const QString& path : waiting) usdcc::usd::openStageFile(path);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::FileOpen) return QObject::eventFilter(watched, event);
        const QString path = static_cast<QFileOpenEvent*>(event)->file();
        if (path.isEmpty()) return QObject::eventFilter(watched, event);
        if (m_ready) usdcc::usd::openStageFile(path);
        else m_waiting << path;
        return true;
    }

private:
    QStringList m_waiting;
    bool m_ready = false;
};

}  // namespace usdcc::app
