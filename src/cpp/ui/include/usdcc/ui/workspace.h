#pragma once

#include "usdcc/core/export.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace usdcc::ui::workspace {

// A workspace file (docs/WORKSPACES.md §3): a named arrangement of view panels,
// read from JSON. Qt-only and knows nothing of the dock manager, so it can be
// tested on its own; WorkspaceApplier puts one on screen.

inline constexpr const char* kFormat = "usdcc-workspace";
inline constexpr int kVersion = 1;
// The registry id of viewports (ViewPanelRegistry): the one panel type the format
// treats specially (§3.4).
inline constexpr const char* kViewportType = "viewport";

// One tab of an area (§3.3).
struct USDCC_UI_API PanelEntry {
    QString panel;      // the ViewPanelRegistry id
    QString title;      // empty: the panel's usual title
    std::optional<bool> optionsPinned;
    QJsonObject state;  // for restoreSessionState(), when the workspace makes the panel
    // Viewports only: "each" (one per open stage, the viewport area) or "current"
    // (one that follows the current stage).
    QString stage = QStringLiteral("each");
    QJsonObject camera;  // viewports only (§3.5)

    bool isViewport() const { return panel == QLatin1String(kViewportType); }
    // The entry that stands for one viewport per open stage.
    bool isPerStageViewport() const { return isViewport() && stage != QLatin1String("current"); }
    bool followsCurrentStage() const { return stage == QLatin1String("current"); }

    QJsonObject toJson() const;
};

// A split (children side by side or stacked) or an area (tabs).
struct USDCC_UI_API Node {
    enum class Kind { Split, Area };
    Kind kind = Kind::Area;

    // Split. Horizontal: side by side, left to right; vertical: top to bottom.
    Qt::Orientation orientation = Qt::Horizontal;
    std::vector<double> sizes;  // normalised shares, one per item (equal when not given)
    std::vector<Node> items;

    // Area.
    std::vector<PanelEntry> tabs;
    int current = -1;       // index of the tab in front; -1: the first (the viewport area: the current stage's)
    QString currentPanel;   // or a panel id

    bool isSplit() const { return kind == Kind::Split; }
    // Every panel entry below this node, in order.
    void forEachEntry(const std::function<void(const PanelEntry&)>& visit) const;
    // Whether this node holds the per-stage viewport entry.
    bool containsPerStageViewport() const;

    QJsonObject toJson() const;
};

struct USDCC_UI_API Definition {
    QString id;
    QString name;
    QString description;
    QString group;
    int order = 1000;
    Node layout;
    // Side panel objectName -> "shown", "collapsed" or "hidden" (§3.2).
    QMap<QString, QString> sidePanels;
    // Fit every viewport placed when switching (§3.5.7): "", "stage" or "selection".
    QString fitOnSwitch;

    // Where it came from, set by the library: the file, and the folder's label
    // ("Your workspaces", "Shipped with usdcc", ...).
    QString file;
    QString source;

    // The JSON (§3), without file/source.
    QJsonObject toJson() const;
};

// Reads a workspace file's JSON. `fallbackId` (the file name without .json) is
// used when it has no "id". Null with `error` saying why when it isn't a
// workspace, is a newer version, or breaks a rule of §3.4.
USDCC_UI_API std::optional<Definition> fromJson(const QJsonObject& json, const QString& fallbackId,
                                                QString* error = nullptr);
// Reads and parses a file; null with `error` (which names the file) otherwise.
USDCC_UI_API std::optional<Definition> readFile(const QString& path, QString* error = nullptr);

// The layout with every entry whose panel type `isKnown` rejects left out, areas left
// with no tabs dropped, and splits left with one child replaced by it (§3.4). The
// per-stage viewport always stays. `missing` gets each unknown type once.
USDCC_UI_API Node pruned(const Node& layout, const std::function<bool(const QString& type)>& isKnown,
                         QStringList* missing = nullptr);

// Whether `id` is a valid workspace id: [A-Za-z0-9._-]+.
USDCC_UI_API bool isValidId(const QString& id);
// An id made from a name: lower case, runs of other characters as "-".
USDCC_UI_API QString idFromName(const QString& name);

}  // namespace usdcc::ui::workspace
