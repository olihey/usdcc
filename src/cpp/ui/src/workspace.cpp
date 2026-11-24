#include "usdcc/ui/workspace.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>
#include <numeric>

namespace usdcc::ui::workspace {

namespace {

const QSet<QString>& sidePanelStates() {
    static const QSet<QString> states{QStringLiteral("shown"), QStringLiteral("collapsed"), QStringLiteral("hidden")};
    return states;
}

const QSet<QString>& sidePanelAreas() {
    static const QSet<QString> areas{QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("top"),
                                     QStringLiteral("bottom")};
    return areas;
}

bool parseSidePanel(const QString& name, const QJsonValue& value, SidePanelEntry* entry, QString* error) {
    QJsonObject object;
    if (value.isString()) {
        object.insert(QStringLiteral("state"), value);
    } else if (value.isObject()) {
        object = value.toObject();
    } else {
        *error = QStringLiteral("side panel \"%1\" is neither a state nor an object").arg(name);
        return false;
    }
    entry->state = object.value(QStringLiteral("state")).toString(QStringLiteral("shown"));
    if (!sidePanelStates().contains(entry->state)) {
        *error = QStringLiteral("side panel \"%1\" is \"%2\", not \"shown\", \"collapsed\" or \"hidden\"")
                     .arg(name, entry->state);
        return false;
    }
    entry->area = object.value(QStringLiteral("area")).toString();
    if (!entry->area.isEmpty() && !sidePanelAreas().contains(entry->area)) {
        *error = QStringLiteral("side panel \"%1\" has the area \"%2\", not \"left\", \"right\", \"top\" or \"bottom\"")
                     .arg(name, entry->area);
        return false;
    }
    const double size = object.value(QStringLiteral("size")).toDouble(0.0);
    entry->size = std::isfinite(size) && size > 0.0 && size < 1.0 ? size : 0.0;
    return true;
}

// "maxWidth" and "maxHeight" of a layout node: whole pixels, 0 (none) when missing.
bool parseMaxSizes(const QJsonObject& object, Node* node, QString* error) {
    for (const auto& [key, target] : {std::pair{QStringLiteral("maxWidth"), &node->maxWidth},
                                      std::pair{QStringLiteral("maxHeight"), &node->maxHeight}}) {
        if (!object.contains(key)) continue;
        const double value = object.value(key).toDouble(-1.0);
        if (!std::isfinite(value) || value < 0.0) {
            *error = QStringLiteral("\"%1\" isn't a number of pixels").arg(key);
            return false;
        }
        *target = int(std::lround(value));
    }
    return true;
}

// The fit names of §3.5.1 and §3.5.7; `true`/`false` as their short forms.
QString fitName(const QJsonValue& value, const QString& trueMeans) {
    if (value.isBool()) return value.toBool() ? trueMeans : QStringLiteral("none");
    return value.toString();
}

std::vector<double> normalised(std::vector<double> sizes, std::size_t count) {
    if (sizes.size() != count) sizes.assign(count, 1.0);
    for (double& size : sizes) {
        if (!std::isfinite(size) || size < 0.0) size = 0.0;
    }
    const double total = std::accumulate(sizes.begin(), sizes.end(), 0.0);
    if (!(total > 0.0)) {
        sizes.assign(count, count ? 1.0 / double(count) : 0.0);
        return sizes;
    }
    for (double& size : sizes) size /= total;
    return sizes;
}

bool parseEntry(const QJsonValue& value, PanelEntry* entry, QString* error) {
    if (!value.isObject()) {
        *error = QStringLiteral("a tab isn't an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    entry->panel = object.value(QStringLiteral("panel")).toString();
    if (entry->panel.isEmpty()) {
        *error = QStringLiteral("a tab has no \"panel\"");
        return false;
    }
    entry->title = object.value(QStringLiteral("title")).toString();
    if (object.value(QStringLiteral("optionsPinned")).isBool()) {
        entry->optionsPinned = object.value(QStringLiteral("optionsPinned")).toBool();
    }
    entry->state = object.value(QStringLiteral("state")).toObject();
    const QString stage = object.value(QStringLiteral("stage")).toString(QStringLiteral("each"));
    if (stage != QLatin1String("each") && stage != QLatin1String("current")) {
        *error = QStringLiteral("\"stage\" is \"%1\", not \"each\" or \"current\"").arg(stage);
        return false;
    }
    entry->stage = stage;
    entry->camera = object.value(QStringLiteral("camera")).toObject();
    if (entry->camera.contains(QStringLiteral("fit"))) {
        // Kept as its name, so the viewport needn't know the short forms.
        entry->camera.insert(QStringLiteral("fit"), fitName(entry->camera.value(QStringLiteral("fit")), QStringLiteral("stage")));
    }
    return true;
}

bool parseNode(const QJsonValue& value, Node* node, QString* error, int depth = 0) {
    if (depth > 32) {
        *error = QStringLiteral("the layout is nested too deep");
        return false;
    }
    if (!value.isObject()) {
        *error = QStringLiteral("a layout node isn't an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    if (object.contains(QStringLiteral("split"))) {
        const QString split = object.value(QStringLiteral("split")).toString();
        if (split != QLatin1String("horizontal") && split != QLatin1String("vertical")) {
            *error = QStringLiteral("\"split\" is \"%1\", not \"horizontal\" or \"vertical\"").arg(split);
            return false;
        }
        node->kind = Node::Kind::Split;
        node->orientation = split == QLatin1String("horizontal") ? Qt::Horizontal : Qt::Vertical;
        const QJsonArray items = object.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) {
            *error = QStringLiteral("a split has no \"items\"");
            return false;
        }
        for (const QJsonValue& item : items) {
            Node child;
            if (!parseNode(item, &child, error, depth + 1)) return false;
            node->items.push_back(std::move(child));
        }
        std::vector<double> sizes;
        for (const QJsonValue& size : object.value(QStringLiteral("sizes")).toArray()) sizes.push_back(size.toDouble(-1.0));
        node->sizes = normalised(std::move(sizes), node->items.size());
        return parseMaxSizes(object, node, error);
    }
    if (object.contains(QStringLiteral("tabs"))) {
        node->kind = Node::Kind::Area;
        const QJsonArray tabs = object.value(QStringLiteral("tabs")).toArray();
        if (tabs.isEmpty()) {
            *error = QStringLiteral("an area has no tabs");
            return false;
        }
        for (const QJsonValue& tab : tabs) {
            PanelEntry entry;
            if (!parseEntry(tab, &entry, error)) return false;
            node->tabs.push_back(std::move(entry));
        }
        const QJsonValue current = object.value(QStringLiteral("current"));
        if (current.isDouble()) {
            node->current = current.toInt(-1);
            if (node->current < 0 || node->current >= int(node->tabs.size())) node->current = -1;
        } else if (current.isString()) {
            node->currentPanel = current.toString();
        }
        return parseMaxSizes(object, node, error);
    }
    *error = QStringLiteral("a layout node is neither a split nor an area of tabs");
    return false;
}

void flatten(Node& node) {
    if (!node.isSplit()) return;
    for (Node& child : node.items) flatten(child);
    // A split of one is its child, with the split's limits where it has none.
    if (node.items.size() == 1) {
        Node child = std::move(node.items.front());
        if (!child.maxWidth) child.maxWidth = node.maxWidth;
        if (!child.maxHeight) child.maxHeight = node.maxHeight;
        node = std::move(child);
        return;
    }
    // A child split in the same direction is part of this one.
    std::vector<Node> items;
    std::vector<double> sizes;
    for (std::size_t i = 0; i < node.items.size(); ++i) {
        Node& child = node.items[i];
        if (child.isSplit() && child.orientation == node.orientation) {
            for (std::size_t j = 0; j < child.items.size(); ++j) {
                items.push_back(std::move(child.items[j]));
                sizes.push_back(node.sizes[i] * child.sizes[j]);
            }
        } else {
            items.push_back(std::move(child));
            sizes.push_back(node.sizes[i]);
        }
    }
    node.items = std::move(items);
    node.sizes = normalised(std::move(sizes), node.items.size());
}

std::optional<Node> prunedNode(const Node& node, const std::function<bool(const QString&)>& isKnown, QStringList* missing) {
    if (!node.isSplit()) {
        Node area = node;
        area.tabs.clear();
        QString currentPanel = node.currentPanel;
        if (currentPanel.isEmpty() && node.current >= 0 && node.current < int(node.tabs.size())) {
            currentPanel = node.tabs[node.current].panel;
        }
        for (const PanelEntry& entry : node.tabs) {
            if (entry.isViewport() || isKnown(entry.panel)) {
                area.tabs.push_back(entry);
            } else if (missing && !missing->contains(entry.panel)) {
                missing->append(entry.panel);
            }
        }
        if (area.tabs.empty()) return std::nullopt;
        area.current = -1;
        area.currentPanel = currentPanel;
        return area;
    }
    Node split = node;
    split.items.clear();
    std::vector<double> sizes;
    for (std::size_t i = 0; i < node.items.size(); ++i) {
        if (std::optional<Node> child = prunedNode(node.items[i], isKnown, missing)) {
            split.items.push_back(std::move(*child));
            sizes.push_back(node.sizes.size() == node.items.size() ? node.sizes[i] : 1.0);
        }
    }
    if (split.items.empty()) return std::nullopt;
    split.sizes = normalised(std::move(sizes), split.items.size());
    flatten(split);
    return split;
}

}  // namespace

QJsonObject PanelEntry::toJson() const {
    QJsonObject object;
    object.insert(QStringLiteral("panel"), panel);
    if (!title.isEmpty()) object.insert(QStringLiteral("title"), title);
    if (optionsPinned) object.insert(QStringLiteral("optionsPinned"), *optionsPinned);
    if (!state.isEmpty()) object.insert(QStringLiteral("state"), state);
    if (isViewport() && followsCurrentStage()) object.insert(QStringLiteral("stage"), stage);
    if (!camera.isEmpty()) object.insert(QStringLiteral("camera"), camera);
    return object;
}

void Node::forEachEntry(const std::function<void(const PanelEntry&)>& visit) const {
    if (isSplit()) {
        for (const Node& child : items) child.forEachEntry(visit);
    } else {
        for (const PanelEntry& entry : tabs) visit(entry);
    }
}

bool Node::hasMaxSize() const {
    if (maxWidth > 0 || maxHeight > 0) return true;
    for (const Node& child : items) {
        if (child.hasMaxSize()) return true;
    }
    return false;
}

int Node::viewportCount(Qt::Orientation along) const {
    if (!isSplit()) {
        return std::any_of(tabs.begin(), tabs.end(), [](const PanelEntry& entry) { return entry.isViewport(); }) ? 1 : 0;
    }
    int count = 0;
    for (const Node& child : items) {
        const int inChild = child.viewportCount(along);
        count = orientation == along ? count + inChild : std::max(count, inChild);
    }
    return count;
}

bool Node::containsPerStageViewport() const {
    bool found = false;
    forEachEntry([&found](const PanelEntry& entry) { found = found || entry.isPerStageViewport(); });
    return found;
}

QJsonObject Node::toJson() const {
    QJsonObject object;
    if (maxWidth > 0) object.insert(QStringLiteral("maxWidth"), maxWidth);
    if (maxHeight > 0) object.insert(QStringLiteral("maxHeight"), maxHeight);
    if (isSplit()) {
        object.insert(QStringLiteral("split"), orientation == Qt::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"));
        QJsonArray shares;
        for (double size : sizes) shares.append(std::round(size * 1000.0) / 1000.0);
        object.insert(QStringLiteral("sizes"), shares);
        QJsonArray children;
        for (const Node& child : items) children.append(child.toJson());
        object.insert(QStringLiteral("items"), children);
        return object;
    }
    QJsonArray list;
    for (const PanelEntry& entry : tabs) list.append(entry.toJson());
    object.insert(QStringLiteral("tabs"), list);
    if (!currentPanel.isEmpty()) {
        object.insert(QStringLiteral("current"), currentPanel);
    } else if (current > 0) {
        object.insert(QStringLiteral("current"), current);
    }
    return object;
}

QJsonValue SidePanelEntry::toJson() const {
    if (area.isEmpty() && size <= 0.0) return state;
    QJsonObject object;
    object.insert(QStringLiteral("state"), state);
    if (!area.isEmpty()) object.insert(QStringLiteral("area"), area);
    if (size > 0.0) object.insert(QStringLiteral("size"), std::round(size * 1000.0) / 1000.0);
    return object;
}

QJsonObject Definition::toJson() const {
    QJsonObject object;
    object.insert(QStringLiteral("format"), QLatin1String(kFormat));
    object.insert(QStringLiteral("version"), kVersion);
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("name"), name);
    if (!description.isEmpty()) object.insert(QStringLiteral("description"), description);
    if (!group.isEmpty()) object.insert(QStringLiteral("group"), group);
    object.insert(QStringLiteral("order"), order);
    if (!fitOnSwitch.isEmpty()) object.insert(QStringLiteral("fitOnSwitch"), fitOnSwitch);
    if (!sidePanels.isEmpty()) {
        QJsonObject panels;
        for (auto it = sidePanels.begin(); it != sidePanels.end(); ++it) panels.insert(it.key(), it.value().toJson());
        object.insert(QStringLiteral("sidePanels"), panels);
    }
    if (!collapsed.empty()) {
        QJsonArray list;
        for (const CollapsedEntry& panel : collapsed) {
            QJsonObject entry = panel.entry.toJson();
            entry.insert(QStringLiteral("area"), panel.area);
            list.append(entry);
        }
        object.insert(QStringLiteral("collapsed"), list);
    }
    object.insert(QStringLiteral("layout"), layout.toJson());
    return object;
}

QStringList Definition::missingTypes(const std::function<bool(const QString&)>& isKnown) const {
    QStringList missing;
    pruned(layout, isKnown, &missing);
    for (const CollapsedEntry& panel : collapsed) {
        if (!isKnown(panel.entry.panel) && !missing.contains(panel.entry.panel)) missing << panel.entry.panel;
    }
    return missing;
}

std::optional<Definition> fromJson(const QJsonObject& json, const QString& fallbackId, QString* error) {
    QString why;
    auto fail = [&](const QString& message) -> std::optional<Definition> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (json.value(QStringLiteral("format")).toString() != QLatin1String(kFormat)) {
        return fail(QStringLiteral("it isn't a workspace (no \"format\": \"%1\")").arg(QLatin1String(kFormat)));
    }
    const int version = json.value(QStringLiteral("version")).toInt(0);
    if (version < 1) return fail(QStringLiteral("it has no \"version\""));
    if (version > kVersion) {
        return fail(QStringLiteral("it is version %1, newer than this app reads (%2)").arg(version).arg(kVersion));
    }

    Definition definition;
    // A copy of Default made while its id was "layout" is still a copy of it.
    definition.id = renamedId(json.value(QStringLiteral("id")).toString(fallbackId));
    if (!isValidId(definition.id)) {
        return fail(QStringLiteral("its id \"%1\" may only hold letters, digits, '.', '_' and '-'").arg(definition.id));
    }
    definition.name = json.value(QStringLiteral("name")).toString().trimmed();
    if (definition.name.isEmpty()) return fail(QStringLiteral("it has no \"name\""));
    definition.description = json.value(QStringLiteral("description")).toString();
    definition.group = json.value(QStringLiteral("group")).toString().trimmed();
    definition.order = json.value(QStringLiteral("order")).toInt(1000);
    if (json.contains(QStringLiteral("fitOnSwitch"))) {
        QString fit = fitName(json.value(QStringLiteral("fitOnSwitch")), QStringLiteral("selection"));
        if (fit == QLatin1String("none")) fit.clear();
        if (!fit.isEmpty() && fit != QLatin1String("stage") && fit != QLatin1String("selection")) {
            return fail(QStringLiteral("\"fitOnSwitch\" is \"%1\", not \"stage\", \"selection\" or a bool").arg(fit));
        }
        definition.fitOnSwitch = fit;
    }
    const QJsonObject sides = json.value(QStringLiteral("sidePanels")).toObject();
    for (auto it = sides.begin(); it != sides.end(); ++it) {
        SidePanelEntry entry;
        if (!parseSidePanel(it.key(), it.value(), &entry, &why)) return fail(why);
        definition.sidePanels.insert(it.key(), entry);
    }

    for (const QJsonValue& value : json.value(QStringLiteral("collapsed")).toArray()) {
        CollapsedEntry panel;
        if (!parseEntry(value, &panel.entry, &why)) return fail(QStringLiteral("\"collapsed\": %1").arg(why));
        if (panel.entry.isViewport()) return fail(QStringLiteral("\"collapsed\" names a viewport, which can't be"));
        panel.area = value.toObject().value(QStringLiteral("area")).toString(panel.area);
        if (!sidePanelAreas().contains(panel.area)) {
            return fail(QStringLiteral("the collapsed panel \"%1\" has the area \"%2\", not \"left\", \"right\", "
                                       "\"top\" or \"bottom\"")
                            .arg(panel.entry.panel, panel.area));
        }
        definition.collapsed.push_back(std::move(panel));
    }

    if (!json.contains(QStringLiteral("layout"))) return fail(QStringLiteral("it has no \"layout\""));
    if (!parseNode(json.value(QStringLiteral("layout")), &definition.layout, &why)) return fail(why);
    flatten(definition.layout);

    int perStage = 0;
    definition.layout.forEachEntry([&perStage](const PanelEntry& entry) { perStage += entry.isPerStageViewport() ? 1 : 0; });
    if (perStage == 0) return fail(QStringLiteral("it has no viewport ({\"panel\": \"viewport\"})"));
    if (perStage > 1) {
        return fail(QStringLiteral("it has %1 per-stage viewports; only one may leave out \"stage\": \"current\"").arg(perStage));
    }
    return definition;
}

std::optional<Definition> readFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("%1: not valid JSON (%2 at offset %3)")
                         .arg(path, parseError.errorString())
                         .arg(parseError.offset);
        }
        return std::nullopt;
    }
    QString why;
    std::optional<Definition> definition = fromJson(document.object(), QFileInfo(path).completeBaseName(), &why);
    if (!definition) {
        if (error) *error = QStringLiteral("%1: %2").arg(path, why);
        return std::nullopt;
    }
    definition->file = QFileInfo(path).absoluteFilePath();
    return definition;
}

Node pruned(const Node& layout, const std::function<bool(const QString&)>& isKnown, QStringList* missing) {
    // The per-stage viewport always stays, so there is always something left.
    std::optional<Node> result = prunedNode(layout, isKnown, missing);
    if (result) return *result;
    Node area;
    area.tabs.push_back(PanelEntry{QLatin1String(kViewportType)});
    return area;
}

bool isValidId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]+$"));
    return pattern.match(id).hasMatch();
}

QString renamedId(const QString& id) { return id == QLatin1String(kFormerDefaultId) ? QString::fromLatin1(kDefaultId) : id; }

QString idFromName(const QString& name) {
    QString id;
    bool dash = false;
    for (const QChar c : name.trimmed().toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
            c == QLatin1Char('.') || c == QLatin1Char('_')) {
            id += c;
            dash = false;
        } else if (!dash && !id.isEmpty()) {
            id += QLatin1Char('-');
            dash = true;
        }
    }
    while (id.endsWith(QLatin1Char('-'))) id.chop(1);
    return id.isEmpty() ? QStringLiteral("workspace") : id;
}

}  // namespace usdcc::ui::workspace
