// view_panel_registry: registration order, lookup, re-registration, and the
// unique-name helper used for panels' objectNames. (Creating and docking a
// panel needs a QApplication and is checked in the app.)
#include "test_harness.h"

#include "usdcc/ui/view_panel_registry.h"

#include <QSet>
#include <QString>

using usdcc::ui::uniqueName;
using usdcc::ui::ViewPanelRegistry;

USDCC_TEST(view_panel_registry) {
    ViewPanelRegistry registry;
    CHECK(registry.entries().empty() && !registry.find(QStringLiteral("viewport")), "starts empty");

    registry.add(QStringLiteral("viewport"), QStringLiteral("Viewport"), {});
    registry.add(QStringLiteral("outliner"), QStringLiteral("Outliner"), {});
    registry.add(QStringLiteral("attributes"), QStringLiteral("Attributes"), {});
    CHECK(registry.entries().size() == 3 && registry.entries()[0].id == QStringLiteral("viewport") &&
              registry.entries()[2].id == QStringLiteral("attributes"),
          "entries keep registration order");

    const ViewPanelRegistry::Entry* outliner = registry.find(QStringLiteral("outliner"));
    CHECK(outliner && outliner->displayName == QStringLiteral("Outliner"), "find by id");
    CHECK(!registry.find(QStringLiteral("nope")), "unknown id");

    registry.add(QStringLiteral("outliner"), QStringLiteral("Scene Tree"), {});
    CHECK(registry.entries().size() == 3 && registry.entries()[1].displayName == QStringLiteral("Scene Tree"),
          "re-registering an id replaces it in place");

    CHECK(!registry.open(QStringLiteral("nope"), nullptr, nullptr, nullptr), "open: unknown id gives null");

    QSet<QString> taken;
    CHECK(uniqueName(QStringLiteral("viewport-3"), [&](const QString& n) { return taken.contains(n); }) ==
              QStringLiteral("viewport-3"),
          "a free name is kept");
    taken << QStringLiteral("viewport-3") << QStringLiteral("viewport-3 (2)");
    CHECK(uniqueName(QStringLiteral("viewport-3"), [&](const QString& n) { return taken.contains(n); }) ==
              QStringLiteral("viewport-3 (3)"),
          "the first free numbered name");
}
