#pragma once

#include "usdcc/core/export.h"

#include <QFrame>

class QAbstractButton;
class QLabel;
class QMenuBar;

namespace usdcc::ui {

// The custom title bar MainWindow shows in place of the operating system's
// (see docs/PLAN.md §6, "Custom title bar"): app logo, the window's menu bar,
// the window title, and minimize / maximize / close buttons, in one row.
//
// Deliberately knows nothing about QWindowKit — it is plain Qt. MainWindow
// registers the widgets below with QWindowKit's agent (which is what makes
// the empty parts of the bar drag the window, and the maximize button show
// Windows 11's snap layouts).
class USDCC_UI_API TitleBar : public QFrame {
    Q_OBJECT

public:
    // `hostWindow` is the top-level window this bar belongs to: it is the
    // bar's parent, what the buttons act on, and whose title / maximized /
    // active state the bar mirrors.
    explicit TitleBar(QWidget* hostWindow);

    QMenuBar* menuBar() const { return m_menuBar; }

    QWidget* logoWidget() const;

    // On macOS, the room at the bar's left end that the system's own close, minimize
    // and zoom buttons are shown in; the bar's own buttons and logo are hidden there,
    // and the menus are in the system's menu bar (docs/MACOS.md). Null elsewhere.
    QWidget* systemButtonArea() const { return m_systemButtonArea; }

    // Adds `widget` right of the title, left of the caption buttons (the
    // workspace button, docs/WORKSPACES.md §6). The bar takes it over. Whoever
    // adds it must also make it clickable for the window agent
    // (MainWindow::addTitleBarWidget() does both).
    void addTrailingWidget(QWidget* widget);
    QAbstractButton* minimizeButton() const { return m_minimizeButton; }
    QAbstractButton* maximizeButton() const { return m_maximizeButton; }
    QAbstractButton* closeButton() const { return m_closeButton; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateWindowState();
    void updateLogo();

    QWidget* m_host;
    QWidget* m_systemButtonArea = nullptr;
    QLabel* m_logo = nullptr;
    QMenuBar* m_menuBar = nullptr;
    QLabel* m_titleLabel = nullptr;
    QAbstractButton* m_minimizeButton = nullptr;
    QAbstractButton* m_maximizeButton = nullptr;
    QAbstractButton* m_closeButton = nullptr;
};

}  // namespace usdcc::ui
