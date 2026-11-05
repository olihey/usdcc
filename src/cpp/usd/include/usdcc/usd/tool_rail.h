#pragma once

#include "usdcc/core/export.h"

#include <QToolBar>

#include <string>

class QActionGroup;

namespace usdcc::usd {

// The vertical strip of tool buttons down the left edge of the main window (the
// design's "left tool rail"): a QToolBar holding one checkable, mutually
// exclusive icon action per tool. Knows nothing about what the tools are —
// callers register them with an id and an icon, and get toolSelected(id) back
// when the user picks one.
class USDCC_USD_UI_API ToolRail : public QToolBar {
    Q_OBJECT

public:
    explicit ToolRail(QWidget* parent = nullptr);

    // Adds an action below the existing ones. `icon` names the pair of theme
    // icons `:/usdcc/theme/<icon>.png` (idle) and `<icon>-active.png` (shown
    // while it's checked, on its accent background), or is the path of an image
    // file, used for both. Empty: the tooltip's first letter is drawn.
    // `shortcutKey` names a Shortcut setting (usdcc::core::Settings) that picks
    // the tool from anywhere in the app; the tooltip shows its current key.
    void addTool(const QString& id, const QString& toolTip, const QString& icon, const std::string& shortcutKey = {});
    // Removes them all.
    void clearTools();

    // Checks the action for `id` without emitting toolSelected().
    void setCurrentTool(const QString& id);

Q_SIGNALS:
    // Only for the user picking a tool, never for setCurrentTool().
    void toolSelected(const QString& id);

private:
    QActionGroup* m_group;
};

}  // namespace usdcc::usd
