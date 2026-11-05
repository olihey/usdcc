#include "usdcc/ui/toast_manager.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scaled_metrics.h"

#include <QEvent>
#include <QMainWindow>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace usdcc::ui {

namespace {

using core::SettingType;
using core::ToastKind;

constexpr int kMargin = 12;   // from the area's edges, at 100% UI scale
constexpr int kSpacing = 8;   // between toasts
constexpr double kMaximumHeight = 0.4;  // of the window's height, per toast

const std::vector<std::string>& cornerNames() {
    static const std::vector<std::string> names = {"Bottom right", "Bottom left", "Top right", "Top left"};
    return names;
}

Qt::WindowFlags overlayFlags() {
    return Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint;
}

}  // namespace

void ToastManager::defineSettings() {
    auto& settings = core::Settings::instance();
    settings.define({.key = positionKey,
                     .category = "Notifications",
                     .group = "Toasts",
                     .label = "Position",
                     .description = "The corner of the main window the toasts stack in.",
                     .type = SettingType::Choice,
                     .defaultValue = cornerNames().front(),
                     .choices = cornerNames()});
    settings.define({.key = durationKey,
                     .category = "Notifications",
                     .group = "Toasts",
                     .label = "Show for (s)",
                     .description = "How long an info or success toast stays.",
                     .type = SettingType::Float,
                     .defaultValue = 5.0,
                     .minimum = 1,
                     .maximum = 60});
    settings.define({.key = warningDurationKey,
                     .category = "Notifications",
                     .group = "Toasts",
                     .label = "Show warnings for (s)",
                     .type = SettingType::Float,
                     .defaultValue = 8.0,
                     .minimum = 1,
                     .maximum = 60});
    settings.define({.key = errorsStickyKey,
                     .category = "Notifications",
                     .group = "Toasts",
                     .label = "Errors stay until dismissed",
                     .description = "Otherwise they go after the warning time.",
                     .type = SettingType::Bool,
                     .defaultValue = true});
    settings.define({.key = maxVisibleKey,
                     .category = "Notifications",
                     .group = "Toasts",
                     .label = "At most on screen",
                     .description = "More wait until others go.",
                     .type = SettingType::Int,
                     .defaultValue = 5,
                     .minimum = 1,
                     .maximum = 10});
}

int ToastManager::defaultDurationMs(core::ToastKind kind) {
    const auto& settings = core::Settings::instance();
    const auto seconds = [&settings](const char* key, double fallback) {
        return int(std::lround(settings.get<double>(key, fallback) * 1000.0));
    };
    switch (kind) {
        case ToastKind::Error:
            if (settings.get<bool>(errorsStickyKey, true)) return core::ToastRequest::kSticky;
            return seconds(warningDurationKey, 8.0);
        case ToastKind::Warning: return seconds(warningDurationKey, 8.0);
        case ToastKind::Info:
        case ToastKind::Success: break;
    }
    return seconds(durationKey, 5.0);
}

ToastManager::ToastManager(QWidget* window) : QObject(window), m_window(window) {
    defineSettings();
    window->installEventFilter(this);

    m_dismissAll = new QToolButton(window);
    m_dismissAll->setWindowFlags(overlayFlags());
    m_dismissAll->setAttribute(Qt::WA_ShowWithoutActivating);
    m_dismissAll->setObjectName(QStringLiteral("toastDismissAll"));
    m_dismissAll->setText(tr("Dismiss All"));
    m_dismissAll->setFocusPolicy(Qt::NoFocus);
    m_dismissAll->hide();
    connect(m_dismissAll, &QToolButton::clicked, this, &ToastManager::dismissAll);

    onSettingChanged(this, positionKey, [this]() { relayout(true); });
    onSettingChanged(this, maxVisibleKey, [this]() { showPending(); });
}

ToastManager::~ToastManager() = default;

ToastManager::Corner ToastManager::corner() const {
    const std::string name = core::Settings::instance().get<std::string>(positionKey);
    const auto& names = cornerNames();
    const auto found = std::find(names.begin(), names.end(), name);
    return found == names.end() ? Corner::BottomRight : static_cast<Corner>(found - names.begin());
}

int ToastManager::show(const core::ToastRequest& request, const QList<ToastAction>& actions) {
    Pending pending{0,
                    request.kind.value_or(ToastKind::Info),
                    QString::fromStdString(request.title),
                    QString::fromStdString(request.body),
                    request.durationMs < 0 ? defaultDurationMs(request.kind.value_or(ToastKind::Info))
                                           : request.durationMs,
                    actions};
    // The same toast again, while it is up (or waiting): counted, not stacked.
    for (ToastWidget* toast : visibleToasts()) {
        if (toast->kind() == pending.kind && toast->title() == pending.title && toast->body() == pending.body) {
            toast->repeat();
            return toast->id();
        }
    }
    for (const Pending& queued : m_queue) {
        if (queued.kind == pending.kind && queued.title == pending.title && queued.body == pending.body) {
            return queued.id;
        }
    }
    pending.id = m_nextId++;
    m_queue.push_back(pending);
    showPending();
    return pending.id;
}

void ToastManager::post(const core::ToastRequest& request, const QList<ToastAction>& actions) {
    QMetaObject::invokeMethod(this, [this, request, actions]() { show(request, actions); }, Qt::QueuedConnection);
}

void ToastManager::dismiss(int id) {
    const auto queued = std::find_if(m_queue.begin(), m_queue.end(), [id](const Pending& p) { return p.id == id; });
    if (queued != m_queue.end()) {
        m_queue.erase(queued);
        return;
    }
    if (ToastWidget* toast = this->toast(id)) toast->dismiss();
}

void ToastManager::dismissAll() {
    m_queue.clear();
    for (ToastWidget* toast : std::as_const(m_toasts)) toast->dismiss();
}

QList<ToastWidget*> ToastManager::visibleToasts() const {
    QList<ToastWidget*> result;
    for (ToastWidget* toast : m_toasts) {
        if (!toast->isClosing()) result.append(toast);
    }
    return result;
}

ToastWidget* ToastManager::toast(int id) const {
    for (ToastWidget* toast : m_toasts) {
        if (toast->id() == id) return toast;
    }
    return nullptr;
}

void ToastManager::showPending() {
    if (!m_window || !m_window->isVisible() || m_window->isMinimized()) return;
    const int limit = core::Settings::instance().get<int>(maxVisibleKey, 5);
    bool opened = false;
    while (!m_queue.empty() && visibleToasts().size() < limit) {
        const Pending pending = m_queue.front();
        m_queue.pop_front();
        open(pending);
        opened = true;
    }
    if (opened) relayout(true);
}

void ToastManager::open(const Pending& pending) {
    const int maximumHeight = int(m_window->height() * kMaximumHeight);
    auto* toast = new ToastWidget(pending.id, pending.kind, pending.title, pending.body, pending.durationMs,
                                  pending.actions, maximumHeight, m_window);
    connect(toast, &ToastWidget::dismissed, this, &ToastManager::onDismissed);
    m_toasts.prepend(toast);
}

void ToastManager::onDismissed(int id) {
    for (int i = 0; i < m_toasts.size(); ++i) {
        if (m_toasts[i]->id() == id) {
            m_toasts.takeAt(i)->deleteLater();
            break;
        }
    }
    showPending();
    relayout(true);
}

QRect ToastManager::area() const {
    if (!m_window) return {};
    QRect local = m_window->rect();
    if (auto* mainWindow = qobject_cast<QMainWindow*>(m_window.data())) {
        if (QWidget* menu = mainWindow->menuWidget(); menu && menu->isVisible()) {
            local.setTop(std::max(local.top(), menu->geometry().bottom() + 1));
        }
        // statusBar() would create one; only look for an existing one.
        if (auto* status = mainWindow->findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly);
            status && status->isVisible()) {
            local.setBottom(std::min(local.bottom(), status->geometry().top() - 1));
        }
        for (QToolBar* bar : mainWindow->findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!bar->isVisible() || bar->isFloating()) continue;
            const QRect geometry = bar->geometry();
            switch (mainWindow->toolBarArea(bar)) {
                case Qt::TopToolBarArea: local.setTop(std::max(local.top(), geometry.bottom() + 1)); break;
                case Qt::BottomToolBarArea: local.setBottom(std::min(local.bottom(), geometry.top() - 1)); break;
                case Qt::LeftToolBarArea: local.setLeft(std::max(local.left(), geometry.right() + 1)); break;
                case Qt::RightToolBarArea: local.setRight(std::min(local.right(), geometry.left() - 1)); break;
                default: break;
            }
        }
    }
    return local.translated(m_window->mapToGlobal(QPoint(0, 0)));
}

void ToastManager::relayout(bool animate) {
    if (!m_window || !m_window->isVisible()) return;
    const QRect bounds = area();
    const Corner where = corner();
    const bool right = where == Corner::BottomRight || where == Corner::TopRight;
    const bool bottom = where == Corner::BottomRight || where == Corner::BottomLeft;
    const int margin = scaled::pixels(kMargin);
    const int spacing = scaled::pixels(kSpacing);

    // Newest nearest the corner; each next one further along the edge.
    const auto place = [&](const QSize& size, int offset) {
        const int x = right ? bounds.right() - margin - size.width() + 1 : bounds.left() + margin;
        const int y = bottom ? bounds.bottom() - margin - offset - size.height() + 1 : bounds.top() + margin + offset;
        return QPoint(x, y);
    };
    int offset = 0;
    const QList<ToastWidget*> toasts = visibleToasts();
    for (ToastWidget* toast : toasts) {
        const QPoint position = place(toast->size(), offset);
        if (!toast->isVisible()) {
            toast->showAt(position);
        } else {
            toast->moveTo(position, animate);
        }
        offset += toast->height() + spacing;
    }

    if (toasts.size() >= 2) {
        m_dismissAll->adjustSize();
        m_dismissAll->move(place(m_dismissAll->size(), offset));
        m_dismissAll->show();
    } else {
        m_dismissAll->hide();
    }
}

void ToastManager::scheduleRelayout() {
    if (m_relayoutScheduled) return;
    m_relayoutScheduled = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_relayoutScheduled = false;
            relayout(false);
        },
        Qt::QueuedConnection);
}

bool ToastManager::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window) {
        switch (event->type()) {
            case QEvent::Show:
            case QEvent::WindowStateChange:
                // Toasts shown before the window was (or while minimized) wait for it.
                QMetaObject::invokeMethod(this, &ToastManager::showPending, Qt::QueuedConnection);
                scheduleRelayout();
                break;
            case QEvent::Move:
            case QEvent::Resize:
                relayout(false);
                break;
            case QEvent::LayoutRequest:  // toolbars docked, moved or shown
                scheduleRelayout();
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace usdcc::ui
