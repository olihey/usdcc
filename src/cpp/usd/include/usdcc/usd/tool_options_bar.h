#pragma once

#include "usdcc/tools/tool.h"

#include <QFrame>

#include <string>
#include <vector>

class QHBoxLayout;

namespace usdcc::usd {

// The contents of the slim bar under the main window's menu bar (the design's
// "tool options bar"): the active tool's name and its settings. It's a plain
// widget that ViewportToolBars hosts inside a QToolBar. It owns no tool state: setTool() rebuilds
// its controls from the tool's own description of its options
// (usdcc::tools::Tool::options()), and changing a control just reports
// optionChanged() for the caller to forward to the tool. A Choice becomes a
// segmented control, a Toggle a checkbox, a Number a spin box; options marked
// enabledBy follow their toggle's state.
class ToolOptionsBar : public QFrame {
    Q_OBJECT

public:
    explicit ToolOptionsBar(QWidget* parent = nullptr);

    // Replaces the bar's contents with `toolName` followed by one control per
    // option. Never emits optionChanged() itself.
    void setTool(const QString& toolName, const std::vector<usdcc::tools::ToolOption>& options);

Q_SIGNALS:
    void optionChanged(const std::string& id, const usdcc::tools::OptionValue& value);

private:
    void clear();
    void addSeparator();
    QWidget* makeChoice(const usdcc::tools::ToolOption& option);
    QWidget* makeToggle(const usdcc::tools::ToolOption& option);
    QWidget* makeNumber(const usdcc::tools::ToolOption& option);

    QHBoxLayout* m_layout;
};

}  // namespace usdcc::usd
