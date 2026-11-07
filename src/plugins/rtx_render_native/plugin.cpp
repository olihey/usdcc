#include "usdcc/plugin/plugin.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProgressDialog>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <limits>

#include "rtx_engine.h"
#include "stage_bridge.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/settings.h"
#include "vulkan_window.h"

namespace usdcc::rtx {
namespace {
struct Roots {
    QString rtx, stage;
};
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
    RtxRenderPanel(usdcc::plugin::Host& host, usdcc::usd::StageRefPtr stage, const QString& title, QWidget* parent)
        : RenderViewPanel(host.stageManager, std::move(stage), title, parent), m_host(host) {
        setRendererName("RTX");
        m_window = new VulkanWindow;
        auto* container = QWidget::createWindowContainer(m_window);
        container->setObjectName("nativeRtxSurface");
        setPresentationWidget(container);
        m_mode = new QComboBox(this);
        m_mode->setObjectName("nativeRtxMode");
        m_mode->addItems({"Real-Time", "Path Tracing", "Minimal"});
        auto& settings = usdcc::core::Settings::instance();
        m_mode->setCurrentText(QString::fromStdString(settings.get<std::string>("RtxRender/mode", "Real-Time")));
        setEditDelay(settings.get<int>("RtxRender/editDelay", 30));
        extraToolbarLayout()->addWidget(m_mode);
        connect(m_mode, &QComboBox::currentTextChanged, this, [this](const QString&) { reload(); });
        m_settings = settings.changed.connect([this](const std::string& key) {
            if (key == "RtxRender/editDelay")
                setEditDelay(usdcc::core::Settings::instance().get<int>(key, 30));
            else if (key == "RtxRender/frameBudget" || key == "RtxRender/sampleBudget")
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
        auto* panel = new RtxRenderPanel(m_host, currentStage(), windowTitle(), parent);
        panel->m_mode->setCurrentIndex(m_mode->currentIndex());
        return panel;
    }

protected:
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
                m_engine = std::make_unique<RtxEngine>(runtime.rtx, runtime.stage, m_window->bridge());
                m_engine->setVisible(!isHidden());
                reload();
            } else if (m_dirty)
                reload();
            if (m_engine) m_engine->setActive(true);
            m_elapsed.start();
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
            auto scene = makeSnapshot(currentStage(), camera, renderSize(), mode());
            m_orthographic = camera.orthographic;
            m_engine->replace(std::move(scene), budget());
            m_engine->setActive(isRendering());
            m_dirty = false;
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
        RenderViewPanel::onStageChanged(stage);
        if (stage && isRendering()) startRender();
    }
    void closing() override {
        m_poll.stop();
        if (m_engine) m_engine->close();
        m_engine.reset();
        m_lastFrame.reset();
        m_window->presented = {};
        m_window->releaseResources();
        if (!VulkanWindow::retirementIdle()) {
            // Keep the HWND alive until its surface and swapchain retire.
            m_window->hide();
            m_window->setParent(static_cast<QWindow*>(nullptr));
            auto* window = m_window;
            auto* timer = new QTimer(QCoreApplication::instance());
            connect(timer, &QTimer::timeout, timer, [window, timer] {
                if (VulkanWindow::retirementIdle()) {
                    timer->stop();
                    delete window;
                    timer->deleteLater();
                }
            });
            timer->start(20);
        }
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
        if (m_engine) m_engine->setVisible(true);
    }

private:
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
    VulkanWindow* m_window{};
    QComboBox* m_mode{};
    QTimer m_poll;
    QElapsedTimer m_elapsed;
    QString m_slowPhase, m_lastStatus;
    bool m_dirty = true, m_orthographic = false;
    std::unique_ptr<RtxEngine> m_engine;
    std::shared_ptr<GpuFrame> m_lastFrame;
    usdcc::core::Signal<const std::string&>::Connection m_settings;
};
// Intercept Close before the host's unsaved-layer questions. Polling a pending
// SDK/driver operation keeps the app responsive; Cancel keeps the app open.
class ShutdownGate final : public QObject {
public:
    explicit ShutdownGate(usdcc::ui::MainWindow* window) : QObject(window), window(window) {
        timer.setInterval(50);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (drained()) {
                timer.stop();
                progress->hide();
                allowNext = true;
                QTimer::singleShot(0, this, [this] { this->window->close(); });
            } else
                progress->setLabelText(
                    QStringLiteral(
                        "Finishing pending RTX work — %1 s. The scene remains available while the native SDK drains.")
                        .arg(elapsed.elapsed() / 1000));
        });
        window->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object != window || event->type() != QEvent::Close) return false;
        if (allowNext) {
            allowNext = false;
            return false;
        }
        RtxEngine::shutdown();
        if (drained()) return false;
        static_cast<QCloseEvent*>(event)->ignore();
        elapsed.start();
        // Construct only after Close: an idle QProgressDialog can otherwise
        // show itself during normal rendering through its automatic timer.
        if (!progress) {
            progress = new QProgressDialog("Finishing pending RTX work…", "Cancel close", 0, 0, window);
            progress->setMinimumDuration(std::numeric_limits<int>::max());
            progress->setWindowModality(Qt::NonModal);
            progress->setAutoClose(false);
            progress->setAutoReset(false);
            connect(progress, &QProgressDialog::canceled, this, [this] {
                timer.stop();
                progress->hide();
            });
        }
        progress->show();
        timer.start();
        return true;
    }

private:
    bool drained() {
        if (!RtxEngine::shutdownFinished()) return false;
        for (auto& panel : panels)
            if (panel) panel->quiesce();
        return VulkanWindow::retirementIdle();
    }
    usdcc::ui::MainWindow* window;
    QProgressDialog* progress{};
    QTimer timer;
    QElapsedTimer elapsed;
    bool allowNext = false;
};
void defineSettings() {
    using namespace usdcc::core;
    auto& settings = Settings::instance();
    auto stored = usdcc::ui::appSettings();
    bool valid = false;
    const int oldBudget = stored.value("RtxRender/maxFrames", 128).toInt(&valid);
    const int legacy = valid ? std::clamp(oldBudget, 1, 4096) : 128;
    // Seed persisted new keys once, preserving existing choices on later runs.
    for (const auto* key : {"RtxRender/frameBudget", "RtxRender/sampleBudget"})
        if (!stored.contains(key)) stored.setValue(key, legacy);
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
}
}  // namespace
}  // namespace usdcc::rtx
USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    usdcc::rtx::defineSettings();
    if (host.mainWindow) new usdcc::rtx::ShutdownGate(host.mainWindow);
    host.addViewPanel("org.usdcc.rtx-render.view", "RTX Render",
                      [&host](const auto& stage, const auto& title, auto* parent) -> usdcc::ui::ViewPanel* {
                          return new usdcc::rtx::RtxRenderPanel(host, stage, title, parent);
                      });
}
USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host&) { usdcc::rtx::RtxEngine::shutdown(); }
