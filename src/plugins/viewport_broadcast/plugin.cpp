// Viewport Broadcast (docs/VIEWPORT_BROADCAST.md, phase 1): Broadcast in a viewport's
// menus hosts a room of usdcc_peers that serves a web page, copies the page's link,
// and sends the viewport's picture to everyone who opens it, whenever it changes.
#include "broadcast.h"

#include "usdcc/plugin/plugin.h"

#include "usdcc/core/toast.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/toast.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>

namespace {

using usdcc::broadcast::Broadcast;
using usdcc::usd::ViewportViewPanel;

// One viewport's picture: what HydraViewportWindow drew last, told by frameRendered().
class ViewportSource : public usdcc::broadcast::FrameSource {
public:
    explicit ViewportSource(ViewportViewPanel* panel) : m_panel(panel) {
        connect(panel->hydraWindow(), &usdcc::usd::HydraViewportWindow::frameRendered, this,
                &FrameSource::frameChanged);
        connect(panel, &usdcc::ui::ViewPanel::stageChanged, this, [this](const usdcc::usd::StageRefPtr& stage) {
            if (!stage) {
                Q_EMIT gone(tr("The stage closed"));
                return;
            }
            Q_EMIT titleChanged();
            Q_EMIT frameChanged();
        });
        connect(panel, &QObject::destroyed, this, [this] { Q_EMIT gone(tr("The viewport closed")); });
    }

    ViewportViewPanel* panel() const { return m_panel; }

    QImage grab() override { return m_panel ? m_panel->hydraWindow()->grabFrame() : QImage(); }

    QString title() const override {
        if (!m_panel) return {};
        const usdcc::usd::StageRefPtr& stage = m_panel->currentStage();
        const QString stageName =
            stage ? QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName()) : QString();
        return stageName.isEmpty() ? m_panel->windowTitle()
                                   : QStringLiteral("%1 · %2").arg(m_panel->windowTitle(), stageName);
    }

private:
    QPointer<ViewportViewPanel> m_panel;
};

// What the plugin keeps: one broadcast at a time, in the first version (§12 question 6).
struct State {
    usdcc::plugin::Host* host = nullptr;
    QPointer<Broadcast> broadcast;
    QPointer<ViewportSource> source;
};
State* g_state = nullptr;

QString setting(const QString& key) { return g_state->host->settingKey(key); }

void toast(const QString& title, const QString& body, const QList<usdcc::ui::ToastAction>& actions = {},
           usdcc::core::ToastKind kind = usdcc::core::ToastKind::Info) {
    if (!g_state) return;
    usdcc::ui::MainWindow* window = g_state->host->mainWindow;
    if (!window || !window->toasts()) return;
    usdcc::core::ToastRequest request;
    request.kind = kind;
    request.title = title.toStdString();
    request.body = body.toStdString();
    window->toasts()->show(request, actions);
}

bool isBroadcasting(const ViewportViewPanel* panel) {
    return g_state->broadcast && g_state->broadcast->isLive() && g_state->source && g_state->source->panel() == panel;
}

// From toasts too, which may outlive the plugin's state at quit.
void copyLink() {
    if (g_state && g_state->broadcast) QGuiApplication::clipboard()->setText(g_state->broadcast->link().toString());
}

void stopBroadcast(const QString& reason = {}) {
    if (g_state && g_state->broadcast) g_state->broadcast->stop(reason);
}

void startBroadcast(ViewportViewPanel* panel) {
    stopBroadcast();  // One at a time: another viewport's ends
    usdcc::plugin::Host& host = *g_state->host;
    if (!host.peers || !host.peers->peers()) {
        toast(QObject::tr("Can't broadcast"), QObject::tr("This app has no Peers."), {}, usdcc::core::ToastKind::Error);
        return;
    }
    const QSettings settings = usdcc::ui::appSettings();
    Broadcast::Options options;
    // On the network by default: Windows asks once whether to let usdcc through. Off, on
    // this machine only (for checks, and for a tunnel run on this machine).
    const bool onNetwork = settings.value(setting(QStringLiteral("onNetwork")), true).toBool() &&
                           !qEnvironmentVariableIsSet("USDCC_BROADCAST_LOOPBACK");
    options.address = onNetwork ? QHostAddress(QHostAddress::Any) : QHostAddress(QHostAddress::LocalHost);
    options.port = static_cast<quint16>(settings.value(setting(QStringLiteral("port")), 47810).toUInt());
    options.pageDirectory = QString::fromStdWString((host.folder / "web").wstring());
    options.allowedOrigins = settings.value(setting(QStringLiteral("allowedOrigins"))).toStringList();

    auto* source = new ViewportSource(panel);
    options.name = QObject::tr("Broadcast: %1").arg(source->title());
    QString error;
    Broadcast* broadcast = Broadcast::start(host.peers->peers(), source, options, &error);
    if (!broadcast) {
        delete source;
        toast(QObject::tr("Can't broadcast"), error, {}, usdcc::core::ToastKind::Error);
        return;
    }
    source->setParent(broadcast);
    g_state->broadcast = broadcast;
    g_state->source = source;
    host.log(usdcc::core::LogLevel::Info, "Broadcasting " + source->title().toStdString() + " on port " +
                                              std::to_string(broadcast->room()->port()));

    QObject::connect(broadcast, &Broadcast::watcherJoined, broadcast, [](const usdcc::peers::Peer& watcher) {
        toast(QObject::tr("%1 is watching").arg(watcher.name), {});
    });
    QObject::connect(broadcast, &Broadcast::stopped, broadcast, [broadcast](const QString& reason) {
        toast(QObject::tr("The broadcast ended"), reason);
        broadcast->deleteLater();
    });

    copyLink();
    QList<usdcc::ui::ToastAction> actions{{QObject::tr("Copy Link"), [] { copyLink(); }}};
    if (host.peers->showPanel) {
        usdcc::ui::PeersController* peers = host.peers;
        actions.append({QObject::tr("Watchers"), [peers] {
                            if (peers->showPanel) peers->showPanel();
                        }});
    }
    actions.append({QObject::tr("Stop"), [] { stopBroadcast(); }});
    toast(QObject::tr("Broadcasting %1").arg(source->title()),
          QObject::tr("The link is copied: whoever opens it in a browser watches this viewport.\n%1")
              .arg(broadcast->link().toString(QUrl::RemoveFragment)),
          actions);
}

}  // namespace

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    g_state = new State;
    g_state->host = &host;
    host.addViewportMenuAction(
        QStringLiteral("Broadcast"),
        [](ViewportViewPanel* panel) {
            if (isBroadcasting(panel)) stopBroadcast();
            else startBroadcast(panel);
        },
        [](ViewportViewPanel* panel) { return isBroadcasting(panel); });
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host&) {
    if (g_state->broadcast) {
        QObject::disconnect(g_state->broadcast, nullptr, nullptr, nullptr);
        delete g_state->broadcast.data();  // Closes the room
    }
    delete g_state;
    g_state = nullptr;
}
