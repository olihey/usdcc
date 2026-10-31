#pragma once

#include <QToolBar>

class QActionGroup;

namespace usdcc::usd {

// The vertical strip of tool buttons down the left edge of the main window (the
// design's "left tool rail"): a QToolBar holding one checkable, mutually
// exclusive icon action per tool. Knows nothing about what the tools are —
// callers register them with an integer id and an icon name, and get
// toolSelected(id) back when the user picks one.
class ToolRail : public QToolBar {
    Q_OBJECT

public:
    explicit ToolRail(QWidget* parent = nullptr);

    // Adds an action below the existing ones. `iconName` names the pair of
    // theme icons `:/usdcc/theme/<iconName>.png` (idle) and `<iconName>-active.png`
    // (shown while it's checked, on its accent background).
    void addTool(int id, const QString& toolTip, const QString& iconName);

    // Checks the action for `id` without emitting toolSelected().
    void setCurrentTool(int id);

Q_SIGNALS:
    // Only for the user picking a tool, never for setCurrentTool().
    void toolSelected(int id);

private:
    QActionGroup* m_group;
};

}  // namespace usdcc::usd
