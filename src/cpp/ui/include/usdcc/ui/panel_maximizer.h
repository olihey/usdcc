#pragma once

#include "usdcc/core/export.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSplitter>
#include <QWidget>

class QMainWindow;

namespace ads {
class CDockAreaWidget;
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace usdcc::ui {

// Temporarily gives one dock area of the main window all the room: double-click
// a ViewPanel's tab, or use its context menu, and again to get the layout back.
// ADS has no such feature of its own.
//
// Everything else is only hidden, not closed or moved: at each splitter between
// the area and the dock manager, the children that don't lead to the area, and
// the side panels docked in the main window. The hidden panels and their
// content (a viewport's renderer, say) stay as they are, and restore() puts the
// splitter sizes and side panels back exactly. Toolbars and floating windows
// stay. The panel's tabs stay too: it's its dock area that is maximized.
//
// Anything that changes the layout meanwhile restores it, so that the change
// doesn't happen out of sight: ADS showing a hidden part again (a hidden panel
// opened), the area being closed, another area or panel being added, and a
// restoreState(). The hidden state must not be saved as the user's layout, so
// MainWindow restores it before saving.
class USDCC_UI_API PanelMaximizer : public QObject {
    Q_OBJECT

public:
    PanelMaximizer(QMainWindow* window, ads::CDockManager* dockManager);

    // Whether `panel` is open and docked in the main window; a floating window
    // has its own maximize button.
    bool canMaximize(const ads::CDockWidget* panel) const;

    bool isActive() const { return m_active; }
    // Whether `panel` is in the maximized area.
    bool isMaximized(const ads::CDockWidget* panel) const;

    // Shows `panel`'s tab and maximizes its area; first restores another one
    // that is maximized. Does nothing unless canMaximize().
    void maximize(ads::CDockWidget* panel);
    // Puts the layout back; does nothing when nothing is maximized.
    void restore();
    // restore() if `panel` is maximized, otherwise maximize().
    void toggle(ads::CDockWidget* panel);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct SplitterSizes {
        QPointer<QSplitter> splitter;
        QList<int> sizes;
    };

    // Not while ADS is in the middle of the change that called for it.
    void restoreLater();

    QMainWindow* m_window;
    ads::CDockManager* m_dockManager;
    bool m_active = false;
    QPointer<ads::CDockAreaWidget> m_area;
    QList<QPointer<QWidget>> m_hiddenWidgets;
    QList<SplitterSizes> m_splitterSizes;  // from the area outwards
    QByteArray m_windowState;              // QMainWindow::saveState()
};

}  // namespace usdcc::ui
