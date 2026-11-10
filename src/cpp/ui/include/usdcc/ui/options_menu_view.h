#pragma once

#include "usdcc/core/export.h"

#include <QWidget>

#include <functional>

class QMenu;
class QVBoxLayout;

namespace usdcc::ui {

// A panel's options menu as widgets, for when it's pinned beside the panel: an action is a
// flat button, one that can be ticked a checkbox, one of a group a radio button, a separator a
// line. A submenu of one choice (its entries all one exclusive group) is a drop-down beside its
// title; any other submenu is a group under its title. Using one triggers its action, as the
// menu would.
//
// The menu is made by `populate`, as each opening of the menu is, and kept for as long as the
// widgets show it, since the actions belong to it. rebuild() makes it again: after an entry is
// used, when an action it shows changes, and when the panel says its options changed.
class USDCC_UI_API OptionsMenuView : public QWidget {
    Q_OBJECT

public:
    explicit OptionsMenuView(std::function<void(QMenu*)> populate, QWidget* parent = nullptr);
    ~OptionsMenuView() override;

    // On the next turn of the event loop, once, however often it's asked.
    void scheduleRebuild();
    void rebuild();
    int rebuildCount() const { return m_rebuilds; }

private:
    void addEntries(QMenu* menu, QVBoxLayout* layout, int depth);
    bool addChoice(QAction* submenuAction, QVBoxLayout* layout);
    bool eventFilter(QObject* watched, QEvent* event) override;

    std::function<void(QMenu*)> m_populate;
    QMenu* m_menu = nullptr;
    QWidget* m_body = nullptr;
    bool m_scheduled = false;
    int m_rebuilds = 0;
};

}  // namespace usdcc::ui
