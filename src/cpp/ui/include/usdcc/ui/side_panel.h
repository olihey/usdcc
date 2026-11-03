#pragma once

#include <QDockWidget>
#include <QSize>

class QMainWindow;

namespace usdcc::ui {

// Base class for panels docked at the edges of the QMainWindow, outside the
// Qt Advanced Docking System container (LogSidePanel, ScriptingSidePanel,
// USDASidePanel — see docs/PLAN.md section 4).
//
// Double-clicking a docked panel's title bar, or its tab when panels share a
// spot (MainWindow routes those), collapses the panel and the panels tabbed
// with it to their title bars; doing it again expands them to the size they
// had. A floating panel's title bar keeps Qt's double-click, which docks it.
class SidePanel : public QDockWidget {
    Q_OBJECT

public:
    explicit SidePanel(const QString& title, QWidget* parent = nullptr);

    bool isCollapsed() const { return m_collapsed; }

    // Collapses or expands this panel together with the panels tabbed with
    // it. Docked in a QMainWindow only; does nothing otherwise. A panel
    // collapses to its title bar's height, except one alone at the left or
    // right edge (not tabbed, nothing stacked above or below it), which
    // collapses to the width of a vertical title bar. `expandedSize` is the
    // height or width expanding goes back to; by default, the current one
    // (it is given when a saved layout is restored).
    void setCollapsed(bool collapsed, int expandedSize = -1);
    void toggleCollapsed() { setCollapsed(!m_collapsed); }

    // The height or width expanding goes back to; while collapsed only.
    int expandedSize() const { return m_expandedSize; }

Q_SIGNALS:
    void collapsedChanged(bool collapsed);

protected:
    bool event(QEvent* event) override;

private:
    QMainWindow* dockedIn() const;
    void collapse(Qt::Orientation orientation, int expandedSize);
    // Returns whether it was collapsed.
    bool expand();

    bool m_collapsed = false;
    Qt::Orientation m_collapsedAlong = Qt::Vertical;  // Vertical: to the title bar's height
    int m_expandedSize = -1;
    bool m_addedVerticalTitleBar = false;
    // From a title bar double-click until the button is released (see event()).
    bool m_ignoreDrag = false;
    // The content's own limits, put back on expanding.
    QSize m_contentMinimum;
    QSize m_contentMaximum;
};

}  // namespace usdcc::ui
