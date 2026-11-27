#include "peers_send.h"

#include "usdcc/peers/room.h"
#include "usdcc/scripting/code_editor.h"
#include "usdcc/scripting/script_editor_side_panel.h"
#include "usdcc/scripting/session_controller.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/render/render_bundle.h"
#include "usdcc/usd/share/prim_text.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>

namespace usdcc::app {

using usdcc::ui::PeersController;

namespace {

bool writeText(const QString& path, const QByteArray& text, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) {
        *error = QObject::tr("Couldn't write %1").arg(path);
        return false;
    }
    return true;
}

// A file name the sender's title doesn't turn into a path.
QString safeName(QString name, const QString& fallback) {
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("_"));
    name = name.trimmed();
    return name.isEmpty() || name.startsWith(u'.') ? fallback : name.left(80);
}

void toast(usdcc::ui::MainWindow* window, const QString& title, const QString& body, bool warning = false) {
    if (!window) {
        return;
    }
    usdcc::core::ToastRequest request;
    request.kind = warning ? usdcc::core::ToastKind::Warning : usdcc::core::ToastKind::Success;
    request.title = title.toStdString();
    request.body = body.toStdString();
    window->toasts()->show(request);
}

QJsonArray vec3(const PXR_NS::GfVec3d& v) {
    return {v[0], v[1], v[2]};
}

}  // namespace

void setUpSendTo(const SendToContext& c, QMenu* menu) {
    PeersController* peers = c.peers;

    // --- What this usdcc does with each kind it receives ---------------------------------

    const auto openFile = [c](const PeersController::ReceivedItem& item) {
        const QString file = QDir(item.directory).filePath(item.data.value("open").toString());
        if (!QFileInfo(file).isFile() || !QFileInfo(file).canonicalFilePath().startsWith(
                                             QFileInfo(item.directory).canonicalFilePath())) {
            toast(c.window, QObject::tr("Couldn't open %1").arg(item.title), QObject::tr("It isn't in what arrived"), true);
            return;
        }
        usdcc::usd::openStageFile(file);
    };
    peers->registerReceiver(QStringLiteral("file"), QObject::tr("Open"), openFile);
    peers->registerReceiver(QStringLiteral("stage"), QObject::tr("Open"), openFile);
    // A stage on a server: both resolve it themselves (docs/REMOTE_ASSETS.md).
    peers->registerReceiver(QStringLiteral("url"), QObject::tr("Open"), [](const PeersController::ReceivedItem& item) {
        usdcc::usd::openStageFile(item.data.value("url").toString());
    });
    peers->registerReceiver(QStringLiteral("prims"), QObject::tr("Paste"), [c](const PeersController::ReceivedItem& item) {
        QFile file(QDir(item.directory).filePath(QStringLiteral("prims.usda")));
        const usdcc::usd::StageRefPtr stage = c.stages->currentStage();
        QString error;
        std::vector<PXR_NS::SdfPath> pasted;
        if (file.open(QIODevice::ReadOnly)) {
            pasted = usdcc::usd::share::pasteUsda(stage, *c.undo, QString::fromUtf8(file.readAll()), &error);
        } else {
            error = QObject::tr("The prims didn't arrive");
        }
        if (pasted.empty()) {
            toast(c.window, QObject::tr("Couldn't paste %1").arg(item.title), error, true);
            return;
        }
        c.stages->setSelectedPaths(stage, pasted);
        toast(c.window, QObject::tr("Pasted %1").arg(item.title),
              QObject::tr("Into %1, from %2").arg(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName()),
                                                  item.from.name));
    });
    peers->registerReceiver(QStringLiteral("session"), QObject::tr("Import and Load"),
                            [c](const PeersController::ReceivedItem& item) {
                                const QString path = QDir(item.directory).filePath(QStringLiteral("session.json"));
                                std::string name;
                                const std::string error = c.sessions->importSession(path.toStdString(), false, &name);
                                if (!error.empty()) {
                                    toast(c.window, QObject::tr("Couldn't import %1").arg(item.title),
                                          QString::fromStdString(error), true);
                                    return;
                                }
                                usdcc::scripting::SessionController::LoadOptions options;
                                options.source = QString::fromStdString(name);
                                QString loadError;
                                if (c.sessions->load(QString::fromStdString(name), options, &loadError) ==
                                    usdcc::scripting::SessionController::LoadResult::Failed) {
                                    toast(c.window, QObject::tr("Couldn't load %1").arg(item.title), loadError, true);
                                }
                            });
    // A script is text in a tab: it is never run (PEERS.md §12).
    peers->registerReceiver(QStringLiteral("script"), QObject::tr("Open in Script Editor"),
                            [c](const PeersController::ReceivedItem& item) {
                                QFile file(QDir(item.directory).filePath(QStringLiteral("script.py")));
                                if (!c.scriptEditor || !file.open(QIODevice::ReadOnly)) {
                                    return;
                                }
                                c.scriptEditor->newScript();
                                if (usdcc::scripting::CodeEditor* editor = c.scriptEditor->editor()) {
                                    editor->setPlainText(QString::fromUtf8(file.readAll()));
                                }
                                c.scriptEditor->renameTab(c.scriptEditor->currentTab(),
                                                          safeName(item.data.value("name").toString(), "received.py"));
                                c.scriptEditor->show();
                                c.scriptEditor->raise();
                            });
    peers->registerReceiver(QStringLiteral("camera"), QObject::tr("Look Through"),
                            [c](const PeersController::ReceivedItem& item) {
                                usdcc::usd::ViewportViewPanel* viewport = usdcc::usd::ViewportViewPanel::activeViewport();
                                if (!viewport) {
                                    toast(c.window, QObject::tr("No viewport to look through"), {}, true);
                                    return;
                                }
                                const QJsonObject json = item.data;
                                usdcc::usd::HydraViewportWindow::CameraState state;
                                const QJsonArray target = json.value("target").toArray();
                                state.target = PXR_NS::GfVec3d(target.at(0).toDouble(), target.at(1).toDouble(),
                                                               target.at(2).toDouble());
                                state.distance = json.value("distance").toDouble(state.distance);
                                state.yaw = json.value("yaw").toDouble(state.yaw);
                                state.pitch = json.value("pitch").toDouble(state.pitch);
                                state.roll = json.value("roll").toDouble();
                                state.orthographic = json.value("orthographic").toBool();
                                state.orthoSize = json.value("orthoSize").toDouble(state.orthoSize);
                                viewport->hydraWindow()->setCameraState(state);
                            });

    // --- File > Send To -------------------------------------------------------------------

    const auto currentStage = [c](QString* error) -> usdcc::usd::StageRefPtr {
        usdcc::usd::StageRefPtr stage = c.stages->currentStage();
        if (!stage) {
            *error = QObject::tr("No stage is open");
        }
        return stage;
    };
    const auto sub = [menu, peers](const QString& title, std::function<std::optional<PeersController::SendItem>(QString*)> make) {
        QMenu* submenu = menu->addMenu(title);
        QObject::connect(submenu, &QMenu::aboutToShow, submenu, [submenu, peers, make] {
            submenu->clear();
            peers->fillSendMenu(submenu, make);
        });
    };

    sub(QObject::tr("Stage File"), [currentStage](QString* error) -> std::optional<PeersController::SendItem> {
        const usdcc::usd::StageRefPtr stage = currentStage(error);
        if (!stage) {
            return std::nullopt;
        }
        const PXR_NS::SdfLayerHandle root = stage->usdStage()->GetRootLayer();
        const QString name = QString::fromStdString(root->GetDisplayName());
        const QString identifier = QString::fromStdString(root->GetIdentifier());
        if (identifier.contains(QStringLiteral("://"))) {
            // On a server: the link is enough (PEERS.md §10.3).
            return PeersController::SendItem{QStringLiteral("url"), name, {{"url", identifier}}, {}, {}};
        }
        const QFileInfo file(QString::fromStdString(root->GetRealPath()));
        if (root->IsAnonymous() || !file.isFile()) {
            *error = QObject::tr("The stage isn't saved to a file; send it with everything it uses");
            return std::nullopt;
        }
        if (root->IsDirty()) {
            *error = QObject::tr("%1 has unsaved edits: save it first, or send it with everything it uses").arg(name);
            return std::nullopt;
        }
        return PeersController::SendItem{QStringLiteral("file"), name, {{"open", file.fileName()}}, file.absolutePath(),
                                         {file.fileName()}};
    });
    sub(QObject::tr("Stage with Everything It Uses"), [currentStage](QString* error) -> std::optional<PeersController::SendItem> {
        const usdcc::usd::StageRefPtr stage = currentStage(error);
        if (!stage) {
            return std::nullopt;
        }
        // Flattened, with its assets beside it, as a render job is (docs/FINAL_RENDER.md §7.3).
        const QString folder = PeersController::newOutboxFolder();
        const PXR_NS::UsdStageRefPtr snapshot = PXR_NS::UsdStage::Open(stage->usdStage()->Flatten());
        if (!snapshot || !usdcc::render::writeSceneBundle(snapshot, folder, error)) {
            return std::nullopt;
        }
        QStringList files;
        QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(QDir(folder).relativeFilePath(it.next()));
        }
        const QString name = QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName());
        return PeersController::SendItem{QStringLiteral("stage"),
                                         QObject::tr("%1 with everything it uses").arg(name),
                                         {{"open", QString::fromLatin1(usdcc::render::kBundleScene)}},
                                         folder,
                                         files};
    });
    sub(QObject::tr("Selected Prims"), [c, currentStage](QString* error) -> std::optional<PeersController::SendItem> {
        const usdcc::usd::StageRefPtr stage = currentStage(error);
        if (!stage) {
            return std::nullopt;
        }
        const std::vector<PXR_NS::SdfPath> paths = c.stages->selectedPaths(stage);
        const QString usda = usdcc::usd::share::primsAsUsda(stage, paths, error);
        if (usda.isEmpty()) {
            if (paths.empty()) {
                *error = QObject::tr("Select prims first");
            }
            return std::nullopt;
        }
        const QString folder = PeersController::newOutboxFolder();
        if (!writeText(QDir(folder).filePath(QStringLiteral("prims.usda")), usda.toUtf8(), error)) {
            return std::nullopt;
        }
        const QString title = paths.size() == 1 ? QString::fromStdString(paths.front().GetName())
                                                : QObject::tr("%1 prims").arg(paths.size());
        return PeersController::SendItem{QStringLiteral("prims"), title, {}, folder, {QStringLiteral("prims.usda")}};
    });
    sub(QObject::tr("This Session"), [c](QString* error) -> std::optional<PeersController::SendItem> {
        // As the app is now (docs/SESSIONS.md §5.1); paths it can't find, the receiver locates.
        const QString name = QObject::tr("%1's session").arg(c.peers->peers()->profile().name);
        const QJsonObject session = c.sessions->capture(name, c.sessions->takePreview());
        const QString folder = PeersController::newOutboxFolder();
        if (!writeText(QDir(folder).filePath(QStringLiteral("session.json")), QJsonDocument(session).toJson(), error)) {
            return std::nullopt;
        }
        return PeersController::SendItem{QStringLiteral("session"), name, {}, folder, {QStringLiteral("session.json")}};
    });
    sub(QObject::tr("Script Editor Tab"), [c](QString* error) -> std::optional<PeersController::SendItem> {
        usdcc::scripting::CodeEditor* editor = c.scriptEditor ? c.scriptEditor->editor() : nullptr;
        if (!editor || editor->toPlainText().isEmpty()) {
            *error = QObject::tr("The Script Editor's tab is empty");
            return std::nullopt;
        }
        const QString folder = PeersController::newOutboxFolder();
        if (!writeText(QDir(folder).filePath(QStringLiteral("script.py")), editor->toPlainText().toUtf8(), error)) {
            return std::nullopt;
        }
        const QString name = c.scriptEditor->currentName();
        return PeersController::SendItem{QStringLiteral("script"), name, {{"name", name}}, folder,
                                         {QStringLiteral("script.py")}};
    });
    sub(QObject::tr("Viewport Camera"), [](QString* error) -> std::optional<PeersController::SendItem> {
        usdcc::usd::ViewportViewPanel* viewport = usdcc::usd::ViewportViewPanel::activeViewport();
        if (!viewport) {
            *error = QObject::tr("No viewport is open");
            return std::nullopt;
        }
        const usdcc::usd::HydraViewportWindow::CameraState state = viewport->hydraWindow()->cameraState();
        const QJsonObject camera{{"target", vec3(state.target)},
                                 {"distance", state.distance},
                                 {"yaw", state.yaw},
                                 {"pitch", state.pitch},
                                 {"roll", state.roll},
                                 {"orthographic", state.orthographic},
                                 {"orthoSize", state.orthoSize},
                                 {"fovY", viewport->hydraWindow()->fieldOfView()}};
        return PeersController::SendItem{QStringLiteral("camera"), QObject::tr("a viewport camera"), camera, {}, {}};
    });
}

}  // namespace usdcc::app
