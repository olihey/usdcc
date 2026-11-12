#include "usdcc/plugin/plugin.h"

#include <QActionGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPointer>
#include <QProgressDialog>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <utility>

#include "rtx_panel.h"
#include "navigation.h"
#include "stage_bridge.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/scene_bounds.h"
#include "usdcc/usd/stage_manager.h"
#include <pxr/usd/usd/stage.h>


namespace usdcc::rtx {
namespace {
struct Roots { QString rtx, stage; };
class RtxRenderPanel;
std::vector<QPointer<RtxRenderPanel>> panels;
Roots roots(const std::filesystem::path& folder) {
    QFile file(QString::fromStdU16String((folder / "runtime.json").u16string()));
    QJsonObject config;
    if (file.open(QIODevice::ReadOnly)) config = QJsonDocument::fromJson(file.readAll()).object();
    Roots result{qEnvironmentVariable("USDCC_RTX_OVRTX_ROOT"), qEnvironmentVariable("USDCC_RTX_OVSTAGE_ROOT")};
    if (result.rtx.isEmpty()) result.rtx = config.value("ovrtx").toString();
    if (result.stage.isEmpty()) result.stage = config.value("ovstage").toString();
    if (result.rtx.isEmpty()) result.rtx = QString::fromStdU16String((folder / "runtime/ovrtx").u16string());
    if (result.stage.isEmpty()) result.stage = QString::fromStdU16String((folder / "runtime/ovstage").u16string());
    return result;
}
class RtxRenderPanel final : public usdcc::usd::RenderViewPanel {
public:
    RtxRenderPanel(usdcc::plugin::Host& host, usdcc::usd::StageRefPtr stage, const QString& title, QWidget* parent, EngineFactory engineFactory, PresentationFactory presentationFactory)
        : RenderViewPanel(host.stageManager, std::move(stage), title, parent), m_host(host), m_engineFactory(std::move(engineFactory)), m_presentationFactory(std::move(presentationFactory)) {
        setRendererName("RTX");
        m_window = m_presentationFactory();
        auto* container = m_window->widget();
        container->setObjectName("nativeRtxSurface");
        container->setFocusPolicy(Qt::StrongFocus);
        setPresentationWidget(container);
        // The mode, as the options menu's Mode shows it (populateRendererOptions()); not shown.
        m_mode = new QComboBox(this);
        m_mode->hide();
        m_mode->setObjectName("nativeRtxMode");
        m_mode->addItems({"Real-Time", "Path Tracing", "Minimal"});
        auto& settings = usdcc::core::Settings::instance();
        m_mode->setCurrentText(QString::fromStdString(settings.get<std::string>("RtxRender/mode", "Real-Time")));
        setEditDelay(settings.get<int>("RtxRender/editDelay", 30));
        connect(m_mode, &QComboBox::currentTextChanged, this, [this](const QString&) {
            notifyOptionsChanged();
            reload();
        });
        if (auto* inputWindow = m_window->window()) {
            m_input = new NavigationInput(inputWindow, this);
            m_input->imageSize = [this] { return m_lastFrame ? m_lastFrame->size() : renderSize(); };
            m_input->begin = [this](bool orbit) { prepareNavigation(); if (orbit) m_navigation.beginOrbit(); };
            m_input->orbit = [this](QPointF delta) { m_navigation.orbit(delta); publishNavigation(); };
            m_input->pan = [this](QPointF delta) { m_navigation.pan(delta); publishNavigation(); };
            m_input->zoom = [this](double steps) { m_navigation.zoom(steps); publishNavigation(); };
            m_input->fit = [this] { frameScene(); };
            m_input->reset = [this] { m_navigation.reset(); publishNavigation(); };
        }
        m_selectionTimer.setSingleShot(true);
        m_selectionTimer.setInterval(kCameraIntervalMs);
        connect(&m_selectionTimer, &QTimer::timeout, this, [this] { refreshSelection(); });
        m_selectionConnection = m_host.stageManager->selectionChanged.connect(
            [this](const auto& stage, const auto&) { if (stage == currentStage()) refreshSelection(); });
        bindSelection();
        m_settings = settings.changed.connect([this](const std::string& key) {
            if (key == "RtxRender/editDelay")
                setEditDelay(usdcc::core::Settings::instance().get<int>(key, 30));
            else if (key == "RtxRender/frameBudget" || key == "RtxRender/sampleBudget" ||
                     key == kMatchStormLightsKey || key == kLightMultiplierKey)
                reload();
        });
        m_window->presented = [this](QSize size, const QString& error) {
            if (!error.isEmpty()) {
                fail(error);
                return;
            }
            showPresentedFrame(size);
            m_lastFrame = m_window->currentFrame();
            setProperty("nativeRtxImageSize", size);
        };
        m_poll.setInterval(30);
        connect(&m_poll, &QTimer::timeout, this, [this] { poll(); });
        setProperty("nativeRtxImageSize", QSize());
        setProperty("nativeRtxFrameSerial", qulonglong(0));
        finishSetup();
        setSettingsCategory("RTX");
        panels.push_back(this);
        m_host.log(usdcc::core::LogLevel::Info, "Native RTX registered; SDK and GPU loading deferred until Render");
    }
    ~RtxRenderPanel() override {
        std::erase_if(panels, [this](const auto& panel) { return !panel || panel == this; });
        closePanel();
    }
    void quiesce() {
        m_poll.stop();
        m_engine.reset();
        m_lastFrame.reset();
        m_window->releaseResources();
        setRendering(false);
        setState("Stopped");
        showPresentedFrame({});
        showMessage("Stopped — press Render to restart RTX.");
        setProperty("nativeRtxImageSize", QSize());
        m_dirty = true;
    }
    usdcc::ui::ViewPanel* duplicate(QWidget* parent) const override {
        auto* panel = new RtxRenderPanel(m_host, currentStage(), windowTitle(), parent, m_engineFactory, m_presentationFactory);
        panel->m_mode->setCurrentIndex(m_mode->currentIndex());
        if (usesLocalCamera()) { panel->m_navigation = m_navigation; panel->publishNavigation(); }
        else {
            auto* from = findChild<QComboBox*>("renderCamera");
            auto* to = panel->findChild<QComboBox*>("renderCamera");
            to->setCurrentIndex(std::max(0, to->findData(from->currentData())));
        }
        return panel;
    }

protected:
    // Mode, Fit to View and Reset Camera, after the app's Camera, Resolution and Update on Edits.
    void populateRendererOptions(QMenu* menu) override {
        QMenu* modes = menu->addMenu("Mode");
        auto* group = new QActionGroup(modes);
        for (int i = 0; i < m_mode->count(); ++i) {
            QAction* action = modes->addAction(m_mode->itemText(i));
            action->setCheckable(true);
            action->setChecked(i == m_mode->currentIndex());
            group->addAction(action);
            connect(action, &QAction::triggered, m_mode, [this, i] { m_mode->setCurrentIndex(i); });
        }
        QAction* fit = menu->addAction("Fit to View", this, [this] { frameScene(); });
        fit->setToolTip("Frame the whole scene in this RTX camera (F over the image)");
        QAction* reset = menu->addAction("Reset Camera", this, [this] { m_navigation.reset(); publishNavigation(); });
        reset->setToolTip("Reset this RTX camera (Home over the image)");
    }
    std::optional<QString> setupProblem() override {
        const auto runtime = roots(m_host.folder);
        for (auto& [name, root] :
             {std::pair{QString("ovrtx"), runtime.rtx}, std::pair{QString("ovstage"), runtime.stage}})
            if (!QFile::exists(root + "/bin/" + name + "-dynamic.dll"))
                return QStringLiteral("Native RTX runtime missing: %1. Set %2 in runtime.json or USDCC_RTX_%3_ROOT.")
                    .arg(root, name, name.toUpper());
        return std::nullopt;
    }
    void startRender() override {
        try {
            if (m_engine && m_engine->isClosed()) m_engine.reset();
            if (!m_engine) {
                const auto device = m_window->initialize();
                if (device.isEmpty()) {
                    fail(m_window->error());
                    return;
                }
                m_host.log(usdcc::core::LogLevel::Info, device.toStdString());
                auto runtime = roots(m_host.folder);
                m_engine = m_engineFactory(runtime.rtx, runtime.stage, m_window->bridge());
                m_engine->setSelection(m_selection);
                m_engine->setVisible(!isHidden());
                reload();
            } else if (m_dirty)
                reload();
            if (m_engine) m_engine->setActive(true);
            m_elapsed.start();
            m_lastStatus.clear();
            m_poll.start();
            setState("Starting RTX…");
        } catch (const std::exception& error) {
            fail(QString::fromUtf8(error.what()));
        }
    }
    void stopRender() override {
        if (m_engine) m_engine->setActive(false);
        setState("Stopped");
    }
    void reload() override {
        m_dirty = true;
        if (!m_engine || !currentStage()) return;
        try {
            const auto camera = currentCamera();
            auto scene = makeSnapshot(currentStage(), camera, renderSize(), mode(), lightMatch());
            m_orthographic = camera.orthographic;
            m_engine->replace(std::move(scene), budget());
            m_engine->setActive(isRendering());
            m_dirty = false;
            m_lastStatus.clear();
            m_elapsed.restart();
            setState(isRendering() ? "Loading the scene…" : "Stopped — changes queued");
        } catch (const std::exception& error) {
            fail(QString::fromUtf8(error.what()));
        }
    }
    void cameraChanged(const usdcc::usd::RenderCamera& camera) override {
        if (!m_engine) {
            m_dirty = true;
            return;
        }
        if (camera.orthographic != m_orthographic) {
            reload();
            return;
        }
        m_engine->update(cameraValues(camera), renderSize());
        m_elapsed.restart();
    }
    void sizeChanged(const QSize&) override { cameraChanged(currentCamera()); }
    void stageEdited(const usdcc::usd::StageChanges& changes) override {
        if (!m_engine) {
            m_dirty = true;
            return;
        }
        auto edits = transformEdits(currentStage(), changes);
        if (!edits || currentCamera().orthographic != m_orthographic) {
            reload();
            return;
        }
        m_engine->update(cameraValues(currentCamera()), renderSize(), *edits);
        m_elapsed.restart();
    }
    void onStageChanged(const usdcc::usd::StageRefPtr& stage) override {
        if (m_engine) {
            m_engine->close();
            m_engine.reset();
        }
        m_window->clear();
        m_lastFrame.reset();
        showPresentedFrame({});
        setProperty("nativeRtxImageSize", QSize());
        m_dirty = true;
        bindSelection();
        RenderViewPanel::onStageChanged(stage);
        if (stage && isRendering()) startRender();
    }
    void closing() override {
        m_selectionTimer.stop();
        m_selectionConnection = {};
        m_selectionStageConnection = {};
        delete std::exchange(m_input, nullptr);
        m_poll.stop();
        if (m_engine) m_engine->close();
        m_engine.reset();
        m_lastFrame.reset();
        m_window->presented = {};
        m_window->close();
        m_settings = {};
    }
    void saveRenderedImage(const QString& path) override {
        if (m_engine && m_lastFrame) {
            m_engine->save(path, m_lastFrame);
            setState("Saving Image…");
            m_poll.start();
        }
    }
    void hideEvent(QHideEvent* event) override {
        if (m_engine) m_engine->setVisible(false);
        RenderViewPanel::hideEvent(event);
    }
    void showEvent(QShowEvent* event) override {
        RenderViewPanel::showEvent(event);
        refreshSelection();
        if (m_engine) m_engine->setVisible(true);
    }

private:
    double cameraAspect() const { const auto size = renderSize(); return double(size.width()) / std::max(1, size.height()); }
    void prepareNavigation() {
        if (!usesLocalCamera())
            m_navigation.seed(currentCamera(), currentStage() ? usdcc::usd::sceneBounds(currentStage()->usdStage(), {})
                                                              : std::nullopt);
        m_navigation.setAspect(cameraAspect());
    }
    void publishNavigation() {
        setLocalCamera(m_navigation.camera(cameraAspect()));
    }
    void frameScene() {
        if (!currentStage()) return;
        const auto bounds = usdcc::usd::sceneBounds(currentStage()->usdStage(), {});
        if (!bounds) { setState("The scene has no visible geometry to frame"); return; }
        prepareNavigation();
        m_navigation.frame(*bounds);
        publishNavigation();
    }
    void refreshSelection() {
        m_selection = selectionGeometry(currentStage(), m_host.stageManager->selectedPaths(currentStage()));
        if (m_engine) m_engine->setSelection(m_selection);
    }
    void bindSelection() {
        m_selectionTimer.stop();
        m_selectionStageConnection = {};
        if (currentStage())
            m_selectionStageConnection = currentStage()->changed.connect([this](const auto& changes) {
                if (!changes.resyncedPaths.empty()) m_selectionTimer.start();
            });
        refreshSelection();
    }
    Mode mode() const {
        return m_mode->currentIndex() == 1   ? Mode::PathTracing
               : m_mode->currentIndex() == 2 ? Mode::Minimal
                                             : Mode::RealTime;
    }
    int budget() const {
        return std::clamp(usdcc::core::Settings::instance().get<int>(
                              mode() == Mode::PathTracing ? "RtxRender/sampleBudget" : "RtxRender/frameBudget", 128),
                          1, 4096);
    }
    static LightMatch lightMatch() {
        auto& settings = usdcc::core::Settings::instance();
        return {.on = settings.get<bool>(kMatchStormLightsKey, true),
                .multiplier = std::clamp(settings.get<double>(kLightMultiplierKey, 1.0), kMinLightMultiplier,
                                         kMaxLightMultiplier)};
    }
    void fail(const QString& error) {
        m_dirty = true;
        if (m_engine) m_engine->setActive(false);
        reportError(error, true);
        m_host.log(usdcc::core::LogLevel::Error, error.toStdString());
    }
    void poll() {
        if (!m_engine) return;
        auto update = m_engine->poll();
        if (update.reloadNeeded) {
            m_host.log(usdcc::core::LogLevel::Warning,
                       "An in-place RTX update failed; replacing the scene from a fresh snapshot");
            reload();
            return;
        }
        if (!update.error.isEmpty()) {
            fail(update.error);
            return;
        }
        if (!update.saveError.isEmpty()) {
            reportError(update.saveError, false);
            m_host.log(usdcc::core::LogLevel::Error, update.saveError.toStdString());
            return;
        }
        if (!update.saved.isEmpty()) {
            setState("Saved " + update.saved);
            return;
        }
        if (update.frame) {
            setProperty("nativeRtxFrameSerial", ++m_frameSerial);
            showMessage({});
            m_window->setFrame(std::move(update.frame));
        }
        if (!update.status.isEmpty() && isRendering()) {
            const bool changed = update.status != m_lastStatus;
            if (changed) { m_lastStatus = update.status; m_elapsed.restart(); }
            if (changed || !update.done) setState(update.status);
            if (!update.done && m_elapsed.isValid() && m_elapsed.elapsed() >= 10000 && update.status != m_slowPhase) {
                m_slowPhase = update.status;
                m_host.log(usdcc::core::LogLevel::Warning,
                           (update.status + ": still pending in the native SDK").toStdString());
            }
        }
    }
    usdcc::plugin::Host& m_host;
    std::unique_ptr<Presentation> m_window;
    EngineFactory m_engineFactory;
    PresentationFactory m_presentationFactory;
    QComboBox* m_mode{};
    QTimer m_poll;
    QElapsedTimer m_elapsed;
    QString m_slowPhase, m_lastStatus;
    bool m_dirty = true, m_orthographic = false;
    qulonglong m_frameSerial = 0;
    std::unique_ptr<Engine> m_engine;
    NavigationCamera m_navigation;
    NavigationInput* m_input{};
    QStringList m_selection;
    QTimer m_selectionTimer;
    usdcc::core::Signal<usdcc::usd::StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    usdcc::core::Signal<const usdcc::usd::StageChanges&>::Connection m_selectionStageConnection;
    std::shared_ptr<GpuFrame> m_lastFrame;
    usdcc::core::Signal<const std::string&>::Connection m_settings;
};
} // namespace
void defineRtxSettings() {
    using namespace usdcc::core;
    auto& settings = Settings::instance();
    auto stored = usdcc::ui::appSettings();
    bool valid = false;
    const int oldBudget = stored.value("RtxRender/maxFrames", 128).toInt(&valid);
    const int legacy = valid ? std::clamp(oldBudget, 1, 4096) : 128;
    // Seed persisted new keys once, preserving existing choices on later runs.
    for (const auto* key : {"RtxRender/frameBudget", "RtxRender/sampleBudget"})
        if (!stored.contains(key)) {
            stored.setValue(key, legacy);
            // A panel test or reactivation may have already defined these keys.
            // Keep the registry consistent with the newly migrated backend value.
            settings.setValue(key, legacy);
        }
    settings.define({.key = "RtxRender/mode",
                     .category = "RTX",
                     .group = "Rendering",
                     .label = "Mode",
                     .description = "Initial mode for new RTX Render panels.",
                     .type = SettingType::Choice,
                     .defaultValue = std::string("Real-Time"),
                     .choices = {"Real-Time", "Path Tracing", "Minimal"}});
    settings.define({.key = "RtxRender/frameBudget",
                     .category = "RTX",
                     .group = "Rendering",
                     .label = "Real-Time / Minimal frames",
                     .type = SettingType::Int,
                     .defaultValue = 128,
                     .minimum = 1,
                     .maximum = 4096});
    settings.define({.key = "RtxRender/sampleBudget",
                     .category = "RTX",
                     .group = "Rendering",
                     .label = "Path Tracing samples",
                     .type = SettingType::Int,
                     .defaultValue = 128,
                     .minimum = 1,
                     .maximum = 4096});
    settings.define({.key = "RtxRender/editDelay",
                     .category = "RTX",
                     .group = "Rendering",
                     .label = "Update after edits (ms)",
                     .type = SettingType::Int,
                     .defaultValue = 30,
                     .minimum = 0,
                     .maximum = 10000});
    settings.define({.key = kMatchStormLightsKey,
                     .category = "RTX",
                     .group = "Lighting",
                     .label = "Match Storm light intensities",
                     .description = "Render at the viewport's brightness: the RTX camera gets unit exposure "
                                    "(f/1, 1 s, ISO 100) instead of ovrtx's default, which is about 1/1134 as "
                                    "bright. Lights aren't changed. Off suits scenes lit for Omniverse.",
                     .type = SettingType::Bool,
                     .defaultValue = true});
    settings.define({.key = kLightMultiplierKey,
                     .category = "RTX",
                     .group = "Lighting",
                     .label = "Intensity multiplier",
                     .description = "While matching Storm: 1 is the viewport's brightness, 2 one stop brighter, "
                                    "0.5 one stop darker.",
                     .type = SettingType::Float,
                     .defaultValue = 1.0,
                     .minimum = kMinLightMultiplier,
                     .maximum = kMaxLightMultiplier});
}
usdcc::usd::RenderViewPanel* createRtxPanel(usdcc::plugin::Host& host, usdcc::usd::StageRefPtr stage,
    const QString& title, QWidget* parent, EngineFactory engine, PresentationFactory presentation) {
    return new RtxRenderPanel(host, std::move(stage), title, parent, std::move(engine), std::move(presentation));
}
void quiesceRtxPanels() { for (auto& panel : panels) if (panel) panel->quiesce(); }
} // namespace usdcc::rtx
