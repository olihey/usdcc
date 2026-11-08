// A view panel's options: the menu, or a panel of its own in a popup. Either can be pinned in
// a column on the panel's left, a menu as widgets, kept current, and remembered across a restart.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/options_menu_view.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage_manager.h"

#include <DockManager.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

using usdcc::ui::ViewPanel;

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 80) QApplication::processEvents();
}

// A panel with the kinds of entries panels' menus have: an option kept by the panel, a
// group of one choice, and an action. And an options panel, when asked to have one.
class TestPanel : public ViewPanel {
public:
    TestPanel(usdcc::usd::StageManager* stages, bool withOptions) : ViewPanel(stages, nullptr, QStringLiteral("Test")), m_withOptions(withOptions) {
        auto* container = new QWidget(this);
        auto* layout = new QVBoxLayout(container);
        auto* toolbar = new QWidget(container);
        auto* toolbarLayout = new QHBoxLayout(toolbar);
        addPanelControls(toolbarLayout);
        layout->addWidget(toolbar);
        content = new QLabel(QStringLiteral("content"), container);
        layout->addWidget(content, 1);
        setContentWidget(container);
        flag = new QAction(QStringLiteral("Show Hidden"), this);
        flag->setCheckable(true);
        refreshStageCombo();
    }
    ViewPanel* duplicate(QWidget*) const override { return new TestPanel(stageManager(), m_withOptions); }
    QToolButton* optionsButton() const { return findChild<QToolButton*>(QStringLiteral("panelMenuButton")); }

    QLabel* content = nullptr;
    QLabel* options = nullptr;
    QAction* flag = nullptr;
    QString renderer = QStringLiteral("Storm");
    int refreshes = 0;

protected:
    void populateOptionsMenu(QMenu* menu) override {
        menu->addAction(flag);
        QMenu* renderers = menu->addMenu(QStringLiteral("Renderer"));
        auto* group = new QActionGroup(renderers);
        for (const QString& name : {QStringLiteral("Storm"), QStringLiteral("Embree")}) {
            QAction* action = renderers->addAction(name);
            action->setCheckable(true);
            action->setChecked(name == renderer);
            group->addAction(action);
            connect(action, &QAction::triggered, this, [this, name]() { renderer = name; });
        }
        menu->addSeparator();
        menu->addAction(QStringLiteral("Refresh"), this, [this]() { ++refreshes; });
    }
    QWidget* createOptionsPanel() override {
        if (!m_withOptions) return nullptr;
        options = new QLabel(QStringLiteral("options"));
        options->setObjectName(QStringLiteral("testOptions"));
        return options;
    }

private:
    bool m_withOptions;
};

struct Window {
    QMainWindow window;
    ads::CDockManager* docks = new ads::CDockManager(&window);
    TestPanel* panel;

    Window(usdcc::usd::StageManager* stages, bool withOptions) {
        panel = new TestPanel(stages, withOptions);
        panel->setObjectName(withOptions ? QStringLiteral("withOptions") : QStringLiteral("withMenu"));
        docks->addDockWidget(ads::CenterDockWidgetArea, panel);
        window.resize(800, 500);
        window.show();
        settle();
    }
};

template <typename T>
T* entry(ViewPanel* panel, const QString& text) {
    if (!panel->pinnedMenu()) return nullptr;
    for (T* widget : panel->pinnedMenu()->findChildren<T*>()) {
        if (widget->text() == text && widget->isVisible()) return widget;
    }
    return nullptr;
}

struct Scale {
    QTemporaryDir temp;
    usdcc::ui::UiScaleController controller{*static_cast<QApplication*>(QCoreApplication::instance()), temp.filePath(QStringLiteral("ui.ini"))};
};
}  // namespace

USDCC_TEST(view_panel_pinned_menu) {
    TestSettings settings;
    Scale scale;
    usdcc::usd::StageManager stages;
    {
        Window w(&stages, false);
        TestPanel* panel = w.panel;
        CHECK(!panel->hasOptionsPanel() && !panel->isOptionsPinned() && !panel->pinnedMenu(), "a menu, not pinned, at first");
        CHECK(panel->contentWidget() && panel->contentWidget()->isAncestorOf(panel->content), "the content is still the content");

        panel->setOptionsPinned(true);
        settle();
        CHECK(panel->isOptionsPinned() && panel->pinnedMenu() && panel->pinnedMenu()->isVisible(), "pinned, the menu shows beside the panel");
        CHECK(panel->optionsButton()->isChecked(), "and the options button stays down");
        CHECK(panel->pinnedMenu()->mapTo(panel, QPoint(0, 0)).x() < panel->content->mapTo(panel, QPoint(0, 0)).x(), "on the left");

        QCheckBox* hidden = entry<QCheckBox>(panel, "Show Hidden");
        QRadioButton* storm = entry<QRadioButton>(panel, "Storm");
        QRadioButton* embree = entry<QRadioButton>(panel, "Embree");
        QPushButton* refresh = entry<QPushButton>(panel, "Refresh");
        CHECK(hidden && !hidden->isChecked(), "an option is a checkbox");
        CHECK(storm && embree && storm->isChecked() && !embree->isChecked(), "a group's choices are radio buttons");
        CHECK(refresh != nullptr, "an action is a button");
        CHECK(!entry<QPushButton>(panel, "Pin to Side"), "and pinning isn't one of them");
        if (!hidden || !embree || !refresh) return;

        // Using them does what the menu would. Each use builds them again, so they're looked up again.
        hidden->click();
        settle();
        CHECK(panel->flag->isChecked(), "ticking an option sets it");
        embree = entry<QRadioButton>(panel, "Embree");
        if (!embree) return;
        embree->click();
        settle();
        CHECK(panel->renderer == "Embree", "choosing in a group chooses: %s", qPrintable(panel->renderer));
        CHECK(entry<QRadioButton>(panel, "Embree") && entry<QRadioButton>(panel, "Embree")->isChecked(), "and shows it chosen");
        refresh = entry<QPushButton>(panel, "Refresh");
        if (!refresh) return;
        refresh->click();
        settle();
        CHECK(panel->refreshes == 1, "a button runs its action");

        // Changes from elsewhere.
        panel->flag->setChecked(false);
        settle();
        CHECK(entry<QCheckBox>(panel, "Show Hidden") && !entry<QCheckBox>(panel, "Show Hidden")->isChecked(), "an option set elsewhere shows at once");
        panel->renderer = QStringLiteral("Storm");
        settle();
        CHECK(entry<QRadioButton>(panel, "Embree") && entry<QRadioButton>(panel, "Embree")->isChecked(), "state the panel keeps doesn't show until it says so");
        if (!panel->pinnedMenu()) return;
        const int before = panel->pinnedMenu()->rebuildCount();
        panel->notifyOptionsChanged();
        panel->notifyOptionsChanged();
        settle();
        CHECK(entry<QRadioButton>(panel, "Storm") && entry<QRadioButton>(panel, "Storm")->isChecked(), "and then does");
        CHECK(panel->pinnedMenu()->rebuildCount() == before + 1, "built once for both: %d", panel->pinnedMenu()->rebuildCount() - before);

        // The button unpins.
        panel->optionsButton()->click();  // a menu would block here, until closed
        settle();
        CHECK(!panel->isOptionsPinned() && !panel->pinnedMenu() && !panel->optionsButton()->isChecked(), "the options button unpins it");
        panel->setOptionsPinned(true);
    }
    {
        Window w(&stages, false);
        CHECK(w.panel->isOptionsPinned() && w.panel->pinnedMenu() && w.panel->pinnedMenu()->isVisible(), "pinned again after a restart");
        auto* unpin = w.panel->findChild<QToolButton*>(QStringLiteral("unpinOptions"));
        CHECK(unpin && unpin->isVisible(), "with a way to unpin it in the column");
        if (unpin) unpin->click();
        settle();
        CHECK(!w.panel->isOptionsPinned(), "which does");
    }
    {
        Window w(&stages, false);
        CHECK(!w.panel->isOptionsPinned(), "and stays unpinned after a restart");
    }
}

USDCC_TEST(view_panel_options_panel) {
    TestSettings settings;
    Scale scale;
    usdcc::usd::StageManager stages;
    {
        Window w(&stages, true);
        TestPanel* panel = w.panel;
        CHECK(panel->hasOptionsPanel(), "it has an options panel");
        CHECK(panel->options && !panel->options->isVisible(), "not shown at first");

        panel->optionsButton()->click();  // a menu would block here, until closed
        settle();
        QWidget* popup = panel->optionsPopup();
        CHECK(popup && popup->isVisible() && popup->isAncestorOf(panel->options) && panel->options->isVisible(), "the button opens it in a popup");
        CHECK(!panel->isOptionsPinned(), "not pinned");

        auto* pin = popup ? popup->findChild<QToolButton*>(QStringLiteral("pinOptions")) : nullptr;
        CHECK(pin != nullptr, "the popup can pin it");
        if (!pin) return;
        pin->click();
        settle();
        CHECK(panel->isOptionsPinned() && !popup->isVisible(), "pinned, the popup goes");
        CHECK(panel->options->isVisible() && !popup->isAncestorOf(panel->options), "and it's beside the panel");
        CHECK(!panel->pinnedMenu(), "itself, not the menu");
        CHECK(panel->options->mapTo(panel, QPoint(0, 0)).x() < panel->content->mapTo(panel, QPoint(0, 0)).x(), "on the left");

        panel->optionsButton()->click();
        settle();
        CHECK(!panel->isOptionsPinned() && !panel->options->isVisible(), "the button unpins it");
        panel->optionsButton()->click();
        settle();
        CHECK(panel->optionsPopup()->isVisible() && panel->options->isVisible(), "and then opens the popup again");
        panel->optionsPopup()->close();
    }
}
