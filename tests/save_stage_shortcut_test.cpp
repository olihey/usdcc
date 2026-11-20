// The Save Stage key (save_stage_as.h, docs/LAYER_EDITOR.md §5): with one stage open
// Ctrl+S saves it, with more it asks which (SaveStageDialog, the current stage picked
// first), and it steps aside where a panel has its own Ctrl+S. Runs in usdcc_ui_tests
// (Qt's offscreen platform).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/toast.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMainWindow>
#include <QShortcut>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <memory>
#include <mutex>

using usdcc::usd::SaveStageDialog;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;

namespace {
QApplication& application() { return *static_cast<QApplication*>(QCoreApplication::instance()); }

void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

QString stageFile(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write("#usda 1.0\n");
    return path;
}

// The toasts posted while it lives: "<kind>|<title>|<message>".
struct ScopedToasts {
    struct Sink : usdcc::core::LogSink {
        std::mutex mutex;
        QStringList toasts;
        void onRecord(const usdcc::core::LogRecord& record) override {
            if (!record.toast) return;
            std::lock_guard lock(mutex);
            const usdcc::core::ToastKind kind = record.toast->kind.value_or(usdcc::core::ToastKind::Info);
            toasts << QStringLiteral("%1|%2|%3").arg(QString::fromStdString(std::string(usdcc::core::toastKindName(kind))),
                                                    QString::fromStdString(record.toast->title),
                                                    QString::fromStdString(record.message));
        }
    };
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    ScopedToasts() { usdcc::core::LogHub::instance().addSink(sink, false); }
    ~ScopedToasts() { usdcc::core::LogHub::instance().removeSink(sink.get()); }
    QString takeLast() {
        std::lock_guard lock(sink->mutex);
        return sink->toasts.isEmpty() ? QString() : sink->toasts.takeLast();
    }
};

// Runs `answer` on the Save Stage dialog that opens next.
void answerNextDialog(std::function<void(SaveStageDialog*)> answer) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, answer]() {
        auto* dialog = qobject_cast<SaveStageDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        poll->stop();
        poll->deleteLater();
        answer(dialog);
    });
    poll->start();
}
}  // namespace

USDCC_TEST(save_stage_dialog_starts_on_the_current_stage) {
    TestSettings testSettings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    const StageRefPtr one = stages.createStage();
    const StageRefPtr two = stages.createStage();
    const StageRefPtr three = stages.createStage();
    stages.setCurrentStage(two);

    SaveStageDialog dialog(&stages);
    auto* choice = dialog.findChild<QComboBox*>(QStringLiteral("saveStageChoice"));
    CHECK(choice && choice->count() == 3, "every open stage to choose from");
    CHECK(dialog.stage() == two, "the current stage picked to start with");
    if (!choice) return;
    choice->setCurrentIndex(2);
    CHECK(dialog.stage() == three, "another one picked");
}

USDCC_TEST(save_stage_shortcut_saves_one_or_asks) {
    TestSettings testSettings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    ScopedToasts toasts;
    CHECK(!usdcc::usd::saveStageFromShortcut(stages, nullptr), "with no stage open, nothing to do");

    // One stage: saved, no question, and a toast says so.
    const QString aPath = stageFile(temp, QStringLiteral("a.usda"));
    const StageRefPtr a = stages.openStage(aPath.toStdString());
    CHECK(a, "a stage file");
    if (!a) return;
    CHECK(usdcc::usd::saveStageFromShortcut(stages, nullptr), "nothing unsaved is no failure");
    QString toast = toasts.takeLast();
    CHECK(toast == "info|Nothing to save|a.usda has no unsaved changes.", "an info toast: %s", qPrintable(toast));
    a->usdStage()->DefinePrim(PXR_NS::SdfPath("/A"));
    CHECK(usdcc::usd::saveStageFromShortcut(stages, nullptr) && !usdcc::usd::stageHasUnsavedLayers(a),
          "the only stage is saved");
    toast = toasts.takeLast();
    CHECK(toast == "success|Saved|Saved a.usda (1 layer).", "a success toast: %s", qPrintable(toast));

    // A file that can't be written: an error toast, still unsaved.
    a->usdStage()->DefinePrim(PXR_NS::SdfPath("/Locked"));
    QFile::setPermissions(aPath, QFile::ReadOwner | QFile::ReadUser);
    CHECK(!usdcc::usd::saveStageFromShortcut(stages, nullptr) && usdcc::usd::stageHasUnsavedLayers(a),
          "a read-only file isn't saved");
    toast = toasts.takeLast();
    CHECK(toast.startsWith("error|Save failed|Could not save a.usda: "), "an error toast: %s", qPrintable(toast));
    QFile::setPermissions(aPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
    CHECK(usdcc::usd::saveStageFromShortcut(stages, nullptr), "writable again, saved");

    // Two: the dialog asks, starting on the current one.
    const StageRefPtr b = stages.openStage(stageFile(temp, QStringLiteral("b.usda")).toStdString());
    CHECK(b, "another stage file");
    if (!b) return;
    a->usdStage()->DefinePrim(PXR_NS::SdfPath("/A2"));
    b->usdStage()->DefinePrim(PXR_NS::SdfPath("/B"));
    stages.setCurrentStage(b);
    StageRefPtr offered;
    answerNextDialog([&offered](SaveStageDialog* dialog) {
        offered = dialog->stage();
        dialog->accept();
    });
    CHECK(usdcc::usd::saveStageFromShortcut(stages, nullptr), "saved");
    CHECK(offered == b, "the current stage offered first");
    CHECK(!usdcc::usd::stageHasUnsavedLayers(b) && usdcc::usd::stageHasUnsavedLayers(a),
          "only the stage picked is saved");
    toast = toasts.takeLast();
    CHECK(toast == "success|Saved|Saved b.usda (1 layer).", "its success toast: %s", qPrintable(toast));

    // Cancel saves nothing.
    answerNextDialog([](SaveStageDialog* dialog) { dialog->reject(); });
    CHECK(!usdcc::usd::saveStageFromShortcut(stages, nullptr) && usdcc::usd::stageHasUnsavedLayers(a),
          "cancelled, nothing saved");
}

USDCC_TEST(save_stage_key_steps_aside_for_a_panels_own) {
    TestSettings testSettings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    QMainWindow window;
    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    auto* plain = new QLineEdit;
    auto* panel = new QWidget;  // as the Layers panel: Ctrl+S of its own
    auto* inner = new QLineEdit(panel);
    (new QVBoxLayout(panel))->addWidget(inner);
    layout->addWidget(plain);
    layout->addWidget(panel);
    window.setCentralWidget(central);
    int panelSaves = 0;
    auto* panelShortcut = new QShortcut(QKeySequence::Save, panel, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    QObject::connect(panelShortcut, &QShortcut::activated, [&panelSaves]() { ++panelSaves; });

    auto* action = new QAction(&window);
    window.addAction(action);
    usdcc::usd::bindSaveStageShortcut(action);
    int stageSaves = 0;
    QObject::connect(action, &QAction::triggered, [&stageSaves]() { ++stageSaves; });
    window.show();
    window.activateWindow();
    settle();

    const QKeySequence ctrlS(Qt::CTRL | Qt::Key_S);
    plain->setFocus();
    settle();
    CHECK(action->shortcut() == ctrlS, "Ctrl+S by default");
    QTest::keyClick(plain, Qt::Key_S, Qt::ControlModifier);
    CHECK(stageSaves == 1 && panelSaves == 0, "outside the panel it saves a stage (%d, %d)", stageSaves, panelSaves);

    inner->setFocus();
    settle();
    CHECK(action->shortcut().isEmpty(), "in the panel the action lets go of the key");
    QTest::keyClick(inner, Qt::Key_S, Qt::ControlModifier);
    CHECK(stageSaves == 1 && panelSaves == 1, "and the panel's own Ctrl+S runs (%d, %d)", stageSaves, panelSaves);

    plain->setFocus();
    settle();
    CHECK(action->shortcut() == ctrlS, "out of the panel it has it again");

    // Another key from Settings: the panel doesn't claim that one.
    usdcc::core::Settings::instance().setValue(usdcc::usd::kSaveStageShortcutKey, std::string("Ctrl+Shift+S"));
    inner->setFocus();
    settle();
    CHECK(action->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), "a new key from Settings, kept in the panel");
    QTest::keyClick(inner, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(stageSaves == 2 && panelSaves == 1, "and it saves a stage there (%d, %d)", stageSaves, panelSaves);
}
