#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/usd_runtime_info.h"

#include <QDialog>
#include <QString>

class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace usdcc::usd {

// Help > USD Runtime (docs/USD_RUNTIME.md §2): the OpenUSD the app runs with,
// in tabs: Overview, Plugins, File Formats, Render Delegates, Environment and
// Debug Flags, whose checkboxes turn TF_DEBUG flags on and off at once. Copy
// puts all of it but the flags on the clipboard as text, for a bug report.
//
// Not modal, so the flags can be changed while working: showDialog() keeps
// one open.
class USDCC_USD_UI_API UsdRuntimeDialog : public QDialog {
    Q_OBJECT
public:
    explicit UsdRuntimeDialog(QWidget* parent = nullptr);

    // The open dialog (created on first use), collected again, shown and
    // brought to the front.
    static UsdRuntimeDialog* showDialog(QWidget* parent);

    const UsdRuntimeInfo& info() const { return m_info; }
    QTabWidget* tabs() const { return m_tabs; }
    // The tab's tree: "overview", "plugins", "fileFormats", "renderers", "environment" or "debugFlags".
    QTreeWidget* tree(const QString& name) const;

    // Collects everything again and fills the tabs.
    void refresh();
    // usdRuntimeInfoText(), as Copy puts it on the clipboard.
    QString text() const;
    void copy();

private:
    void fill();
    void fillDebugFlags();
    void setDebugFlag(const QString& name, bool enabled);

    UsdRuntimeInfo m_info;
    QTabWidget* m_tabs = nullptr;
    QPushButton* m_copy = nullptr;
    bool m_filling = false;
};

}  // namespace usdcc::usd
