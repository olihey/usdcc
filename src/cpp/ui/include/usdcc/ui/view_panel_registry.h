#pragma once

#include "usdcc/usd/stage.h"

#include <QString>

#include <functional>
#include <vector>

class QWidget;

namespace ads {
class CDockManager;
}

namespace usdcc::ui {

class ViewPanel;

// The ViewPanel types the app can create on demand — what the Stages menu
// offers for each open stage. The app registers its types at startup (their
// constructors need app-level objects such as the tool set or undo manager,
// which the factories capture); anything listing panel types reads them from
// here, so a type registered later shows up without further changes.
class ViewPanelRegistry {
public:
    // Creates a panel showing `stage`, titled `title`, parented to `parent`;
    // the caller docks it.
    using Factory = std::function<ViewPanel*(const usdcc::usd::StageRefPtr& stage, const QString& title,
                                             QWidget* parent)>;

    struct Entry {
        QString id;           // stable, e.g. "viewport"; also the base of objectNames
        QString displayName;  // shown to the user, e.g. "Viewport"
        Factory create;
    };

    // Registers a type; registering an id again replaces it in place.
    void add(const QString& id, const QString& displayName, Factory create);

    // In registration order.
    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry* find(const QString& id) const;

    // Creates a panel of type `id` for `stage` and docks it in `dockManager`:
    // as a tab next to a panel of the same class that is on screen, otherwise
    // in the centre area, then brings it to the front. It is titled
    // "<display name> - <stage file name>" and gets a unique objectName (ADS's
    // identity for a dock widget). Returns null for an unknown id.
    ViewPanel* open(const QString& id, const usdcc::usd::StageRefPtr& stage, ads::CDockManager* dockManager,
                    QWidget* parent) const;

private:
    std::vector<Entry> m_entries;
};

// `base` if `taken(base)` is false, else the first free "base (2)",
// "base (3)", ...
QString uniqueName(const QString& base, const std::function<bool(const QString&)>& taken);

}  // namespace usdcc::ui
