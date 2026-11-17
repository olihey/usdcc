// File > Close Stage and its Close All (close_stage_dialog.h): the question
// about the panels still showing the closing stages offers only stages that
// stay open, so just "No stage" when every stage closes. Runs in
// usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <DockManager.h>

#include <QApplication>
#include <QComboBox>
#include <QPointer>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

using usdcc::ui::MainWindow;
using usdcc::usd::CloseStageDialog;
using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;

namespace {
// Settings in Qt's test-mode folder, never the user's.
struct TestMode {
    TestMode() { QStandardPaths::setTestModeEnabled(true); }
    ~TestMode() { QStandardPaths::setTestModeEnabled(false); }
};

QStringList choices(const CloseStageDialog& dialog) {
    QStringList texts;
    if (auto* combo = dialog.findChild<QComboBox*>("closeStageReplacement")) {
        for (int i = 0; i < combo->count(); ++i) texts << combo->itemText(i);
    }
    return texts;
}

// Runs `answer` on the Close Stage dialog that opens next.
void answerNextDialog(std::function<void(CloseStageDialog*)> answer) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, answer]() {
        auto* dialog = qobject_cast<CloseStageDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        poll->stop();
        poll->deleteLater();
        answer(dialog);
    });
    poll->start();
}
}  // namespace

USDCC_TEST(close_stage_dialog_offers_stages_staying_open) {
    StageManager stages;
    const StageRefPtr one = stages.createStage();
    const StageRefPtr two = stages.createStage();
    const StageRefPtr three = stages.createStage();

    CloseStageDialog single(&stages, one, {QStringLiteral("Viewport")});
    const QStringList others = choices(single);
    CHECK(others.size() == 3 && others.last() == "No stage", "closing one: the two others, then No stage: %s",
          qPrintable(others.join(", ")));
    CHECK(single.windowTitle() == "Close Stage", "titled for one");

    CloseStageDialog pair(&stages, std::vector<StageRefPtr>{one, two}, {QStringLiteral("Viewport")});
    CHECK(choices(pair).size() == 2 && choices(pair).last() == "No stage", "closing two: the third, then No stage");
    pair.findChild<QComboBox*>("closeStageReplacement")->setCurrentIndex(0);
    CHECK(pair.replacement() == three, "which switches the panels to it");

    CloseStageDialog all(&stages, stages.stages(), {QStringLiteral("Viewport")});
    CHECK(choices(all) == QStringList{"No stage"}, "Close All: only No stage: %s", qPrintable(choices(all).join(", ")));
    CHECK(!all.replacement(), "which leaves the panels empty");
    CHECK(all.windowTitle() == "Close Stages", "titled for several");
}

USDCC_TEST(close_all_stages) {
    TestMode testMode;
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr one = stages.createStage();
    const StageRefPtr two = stages.createStage();

    MainWindow window;
    ads::CDockManager* docks = window.dockManager();
    QPointer<OutlinerViewPanel> a = new OutlinerViewPanel(&stages, one, undo, QStringLiteral("A"), &window);
    QPointer<OutlinerViewPanel> b = new OutlinerViewPanel(&stages, two, undo, QStringLiteral("B"), &window);
    docks->addDockWidget(ads::LeftDockWidgetArea, a);
    docks->addDockWidget(ads::RightDockWidgetArea, b);
    window.resize(900, 500);
    window.show();
    QApplication::processEvents();

    // Cancel keeps everything.
    QStringList offered;
    answerNextDialog([&offered](CloseStageDialog* dialog) {
        offered = choices(*dialog);
        dialog->reject();
    });
    CHECK(!usdcc::usd::closeStagesInteractively(&stages, stages.stages(), &window), "cancelled");
    CHECK(offered == QStringList{"No stage"}, "one question for both, offering only No stage: %s", qPrintable(offered.join(", ")));
    CHECK(stages.stages().size() == 2 && a->currentStage() == one && b->currentStage() == two, "nothing closed");

    // Switching to No stage: every stage closes, the panels stay, empty.
    answerNextDialog([](CloseStageDialog* dialog) { dialog->accept(); });
    CHECK(usdcc::usd::closeStagesInteractively(&stages, stages.stages(), &window), "closed");
    CHECK(stages.stages().empty(), "every stage");
    CHECK(a && b && !a->currentStage() && !b->currentStage(), "the panels stay, with no stage");
}
