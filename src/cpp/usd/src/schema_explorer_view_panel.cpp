#include "usdcc/usd/schema_explorer_view_panel.h"

#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_view.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace usdcc::usd {

namespace {
constexpr int kNameRole = Qt::UserRole;  // a schema's name; empty on a category row

enum Column { Name, Kind, ColumnCount };
enum PropertyColumn { PropertyName, PropertyType, Fallback, DeclaredBy, PropertyColumnCount };

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString escaped(const std::string& text) { return qs(text).toHtmlEscaped(); }

// Schema documentation is plain text with Doxygen markup in it here and there;
// paragraphs are separated by blank lines.
QString documentationHtml(const std::string& text) {
    QString html;
    const QStringList paragraphs = qs(text).split(QStringLiteral("\n\n"), Qt::SkipEmptyParts);
    for (const QString& paragraph : paragraphs) {
        const QString simplified = paragraph.simplified();
        if (!simplified.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(simplified.toHtmlEscaped());
    }
    return html;
}

bool matchesShow(const SchemaInfo& info, SchemaExplorerViewPanel::Show show) {
    switch (show) {
        case SchemaExplorerViewPanel::Show::All: return true;
        case SchemaExplorerViewPanel::Show::PrimTypes: return info.kind == SchemaInfo::Kind::ConcreteTyped;
        case SchemaExplorerViewPanel::Show::AbstractTypes:
            return info.kind == SchemaInfo::Kind::AbstractTyped || info.kind == SchemaInfo::Kind::AbstractBase;
        case SchemaExplorerViewPanel::Show::APISchemas: return isAPI(info.kind);
        case SchemaExplorerViewPanel::Show::Codeless: return info.codeless;
    }
    return true;
}

// The tooltip of a property row: what doesn't fit in the columns.
QString propertyToolTip(const SchemaProperty& property) {
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(escaped(property.name));
    if (!property.displayName.empty() || !property.displayGroup.empty()) {
        lines << QObject::tr("Shown as \"%1\"%2")
                     .arg(escaped(property.displayName.empty() ? property.name : property.displayName),
                          property.displayGroup.empty() ? QString()
                                                        : QObject::tr(" in %1").arg(escaped(property.displayGroup)));
    }
    if (property.uniform) lines << QObject::tr("Uniform: one value, no time samples");
    if (!property.allowedTokens.empty()) {
        QStringList tokens;
        for (const std::string& token : property.allowedTokens) tokens << qs(token);
        lines << QObject::tr("Allowed: %1").arg(tokens.join(QStringLiteral(", ")).toHtmlEscaped());
    }
    QString html = lines.join(QStringLiteral("<br>"));
    if (!property.documentation.empty()) html += documentationHtml(property.documentation);
    return html;
}
}  // namespace

SchemaExplorerViewPanel::SchemaExplorerViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& title,
                                                 QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    m_filter = new QLineEdit(toolbar);
    m_filter->setObjectName(QStringLiteral("schemaFilter"));
    m_filter->setPlaceholderText(tr("Filter schemas"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setToolTip(tr("Show the schemas whose name has this in it"));
    toolbarLayout->addWidget(m_filter, 1);
    m_show = new QComboBox(toolbar);
    m_show->setObjectName(QStringLiteral("schemaShow"));
    m_show->addItem(tr("All"), int(Show::All));
    m_show->addItem(tr("Prim Types"), int(Show::PrimTypes));
    m_show->addItem(tr("Abstract Types"), int(Show::AbstractTypes));
    m_show->addItem(tr("API Schemas"), int(Show::APISchemas));
    m_show->addItem(tr("Codeless"), int(Show::Codeless));
    m_show->setToolTip(tr("Which schemas to list. Codeless ones come from a plugin with no library"));
    toolbarLayout->addWidget(m_show);
    m_summary = new QLabel(toolbar);
    toolbarLayout->addWidget(m_summary);

    m_tree = new QTreeWidget(container);
    m_tree->setObjectName(QStringLiteral("schemaTree"));
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Schema"), tr("Kind")});
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(Kind, QHeaderView::ResizeToContents);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SchemaExplorerViewPanel::showMenu);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() { showDetails(); });

    m_details = new QTextBrowser(container);
    m_details->setObjectName(QStringLiteral("schemaDetails"));
    m_details->setOpenLinks(false);
    m_details->setFrameShape(QFrame::NoFrame);
    connect(m_details, &QTextBrowser::anchorClicked, this, &SchemaExplorerViewPanel::openLink);

    m_properties = new QTreeWidget(container);
    m_properties->setObjectName(QStringLiteral("schemaProperties"));
    m_properties->setColumnCount(PropertyColumnCount);
    m_properties->setHeaderLabels({tr("Property"), tr("Type"), tr("Fallback"), tr("Declared by")});
    m_properties->setRootIsDecorated(false);
    m_properties->setUniformRowHeights(true);
    m_properties->setAlternatingRowColors(true);
    m_properties->header()->setStretchLastSection(true);
    m_properties->setToolTip(tr("Double-click a row to go to the schema that declares it"));
    connect(m_properties, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item) selectSchema(item->text(DeclaredBy));
    });

    auto* right = new QSplitter(Qt::Vertical, container);
    right->addWidget(m_details);
    right->addWidget(m_properties);
    right->setStretchFactor(0, 2);
    right->setStretchFactor(1, 3);
    auto* split = new QSplitter(Qt::Horizontal, container);
    split->addWidget(m_tree);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    layout->addWidget(toolbar);
    layout->addWidget(split, 1);
    setContentWidget(container);

    connect(m_filter, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(m_show, &QComboBox::currentIndexChanged, this, [this]() { applyFilter(); });

    refreshStageCombo();
    refresh();
}

usdcc::ui::ViewPanel* SchemaExplorerViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new SchemaExplorerViewPanel(stageManager(), currentStage(), windowTitle(), parent);
    copy->restoreSessionState(saveSessionState());
    return copy;
}

QJsonObject SchemaExplorerViewPanel::saveSessionState() const {
    QJsonObject state;
    if (const SchemaInfo* info = currentSchema()) state[QStringLiteral("schema")] = qs(info->name);
    if (!m_filter->text().isEmpty()) state[QStringLiteral("filter")] = m_filter->text();
    if (m_show->currentIndex() > 0) state[QStringLiteral("show")] = m_show->currentIndex();
    return state;
}

void SchemaExplorerViewPanel::restoreSessionState(const QJsonObject& state) {
    m_show->setCurrentIndex(std::clamp(state.value(QStringLiteral("show")).toInt(), 0, m_show->count() - 1));
    m_filter->setText(state.value(QStringLiteral("filter")).toString());
    const QString schema = state.value(QStringLiteral("schema")).toString();
    if (!schema.isEmpty()) selectSchema(schema);  // a schema that's gone is skipped
}

const SchemaInfo* SchemaExplorerViewPanel::find(const std::string& name) const {
    const auto it = m_index.find(name);
    return it == m_index.end() ? nullptr : &m_schemas[it->second];
}

const SchemaInfo* SchemaExplorerViewPanel::currentSchema() const {
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item ? find(item->data(Name, kNameRole).toString().toStdString()) : nullptr;
}

bool SchemaExplorerViewPanel::selectSchema(const QString& name) {
    // "CollectionAPI:lightLink" is CollectionAPI's.
    const std::string schema = name.section(QLatin1Char(':'), 0, 0).toStdString();
    const auto it = m_items.find(schema);
    if (it == m_items.end()) return false;
    if (it->second->isHidden()) {
        // Hidden by the filter: clear it rather than not go there.
        const QSignalBlocker blocker(m_show);
        m_show->setCurrentIndex(0);
        m_filter->clear();
        applyFilter();
    }
    for (QTreeWidgetItem* parent = it->second->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
    m_tree->setCurrentItem(it->second);
    m_tree->scrollToItem(it->second);
    return true;
}

void SchemaExplorerViewPanel::setFilterText(const QString& text) { m_filter->setText(text); }

void SchemaExplorerViewPanel::setShow(Show show) { m_show->setCurrentIndex(m_show->findData(int(show))); }

void SchemaExplorerViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    QAction* refreshAction = menu->addAction(tr("Refresh"), this, &SchemaExplorerViewPanel::refresh);
    refreshAction->setToolTip(tr("Read USD's schema registry again"));
    menu->addAction(tr("Expand All"), m_tree, &QTreeWidget::expandAll);
    menu->addAction(tr("Collapse All"), m_tree, &QTreeWidget::collapseAll);
}

void SchemaExplorerViewPanel::refresh() {
    const QString current = currentSchema() ? qs(currentSchema()->name) : QString();
    m_schemas = allSchemas();
    m_index.clear();
    for (std::size_t i = 0; i < m_schemas.size(); ++i) m_index[m_schemas[i].name] = i;
    rebuildTree();
    applyFilter();
    if (current.isEmpty() || !selectSchema(current)) showDetails();
}

void SchemaExplorerViewPanel::rebuildTree() {
    const QSignalBlocker blocker(m_tree);
    m_tree->clear();
    m_items.clear();

    // Categories by name; in each, typed schemas nested under their base when it
    // is in the same category, then the API schemas.
    std::map<std::string, std::vector<const SchemaInfo*>> categories;
    std::map<std::string, std::set<std::string>> plugins;
    for (const SchemaInfo& info : m_schemas) {
        categories[info.category].push_back(&info);
        plugins[info.category].insert(info.plugin);
    }
    for (const auto& [category, members] : categories) {
        auto* row = new QTreeWidgetItem(m_tree, {qs(category)});
        QStringList names;
        bool codeless = false;
        for (const std::string& plugin : plugins[category]) names << qs(plugin);
        for (const SchemaInfo* info : members) codeless = codeless || info->codeless;
        row->setToolTip(Name, tr("From %1").arg(names.join(QStringLiteral(", "))));
        if (codeless) row->setText(Kind, tr("codeless"));
        QFont font = row->font(Name);
        font.setBold(true);
        row->setFont(Name, font);
        row->setFlags(row->flags() & ~Qt::ItemIsSelectable);

        // Typed first, so a base exists before what derives from it is placed:
        // a base's name sorts anywhere, so place in passes until nothing moves.
        std::vector<const SchemaInfo*> typed, apis;
        for (const SchemaInfo* info : members) (isTyped(info->kind) || info->kind == SchemaInfo::Kind::AbstractBase ? typed : apis).push_back(info);
        std::set<std::string> inCategory;
        for (const SchemaInfo* info : typed) inCategory.insert(info->name);
        std::vector<const SchemaInfo*> pending = typed;
        while (!pending.empty()) {
            std::vector<const SchemaInfo*> later;
            for (const SchemaInfo* info : pending) {
                QTreeWidgetItem* parent = row;
                if (inCategory.count(info->base)) {
                    const auto base = m_items.find(info->base);
                    if (base == m_items.end()) {
                        later.push_back(info);
                        continue;
                    }
                    parent = base->second;
                }
                auto* item = new QTreeWidgetItem(parent, {qs(info->name), qs(schemaKindName(info->kind))});
                item->setData(Name, kNameRole, qs(info->name));
                item->setIcon(Name, prim_icons::icon(prim_icons::iconNameForType(PXR_NS::TfToken(info->name))));
                m_items[info->name] = item;
            }
            if (later.size() == pending.size()) break;  // a cycle can't happen; never loop on one
            pending = std::move(later);
        }
        for (const SchemaInfo* info : apis) {
            auto* item = new QTreeWidgetItem(row, {qs(info->name), qs(schemaKindName(info->kind))});
            item->setData(Name, kNameRole, qs(info->name));
            m_items[info->name] = item;
        }
    }
    for (const auto& [name, item] : m_items) {
        if (const SchemaInfo* info = find(name)) {
            const QString firstParagraph = qs(info->documentation).section(QStringLiteral("\n\n"), 0, 0).simplified();
            item->setToolTip(Name, firstParagraph.isEmpty() ? qs(info->name) : firstParagraph);
        }
    }
}

void SchemaExplorerViewPanel::applyFilter() {
    const QString text = m_filter->text().trimmed();
    const Show show = Show(m_show->currentData().toInt());
    const bool filtering = !text.isEmpty() || show != Show::All;
    int shown = 0;
    // An item shows when it matches, or something under it does.
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        bool any = false;
        for (int i = 0; i < item->childCount(); ++i) any = visit(item->child(i)) || any;
        bool matches = false;
        if (const SchemaInfo* info = find(item->data(Name, kNameRole).toString().toStdString())) {
            matches = matchesShow(*info, show) &&
                      (text.isEmpty() || qs(info->name).contains(text, Qt::CaseInsensitive) ||
                       qs(info->typeName).contains(text, Qt::CaseInsensitive));
            if (matches) ++shown;
        }
        item->setHidden(!matches && !any);
        // An ancestor shown only for a match below it is dimmed.
        item->setForeground(Name, matches || !item->data(Name, kNameRole).isValid() || !filtering
                                      ? QBrush()
                                      : m_tree->palette().brush(QPalette::Disabled, QPalette::Text));
        if (filtering && any) item->setExpanded(true);
        return matches || any;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) visit(m_tree->topLevelItem(i));
    m_summary->setText(filtering ? tr("%1 of %2").arg(shown).arg(m_schemas.size()) : tr("%1 schemas").arg(m_schemas.size()));
}

QString SchemaExplorerViewPanel::schemaLink(const std::string& name) const {
    const std::string schema = name.substr(0, name.find(':'));
    if (!find(schema)) return escaped(name);  // not registered: plain text
    return QStringLiteral("<a href=\"schema:%1\">%2</a>").arg(escaped(schema), escaped(name));
}

QString SchemaExplorerViewPanel::detailsHtml(const SchemaInfo& info) const {
    auto links = [this](const std::vector<std::string>& names) {
        QStringList parts;
        for (const std::string& name : names) parts << schemaLink(name);
        return parts.join(QStringLiteral(", "));
    };
    QString html = QStringLiteral("<h3>%1</h3>").arg(escaped(info.name));
    QStringList facts{qs(schemaKindName(info.kind))};
    if (!info.plugin.empty()) facts << (info.codeless ? tr("%1, codeless").arg(escaped(info.plugin)) : escaped(info.plugin));
    if (info.version > 0) facts << tr("version %1 of %2").arg(info.version).arg(schemaLink(info.family));
    facts << QStringLiteral("<code>%1</code>").arg(escaped(info.typeName));
    html += QStringLiteral("<p>%1</p>").arg(facts.join(QStringLiteral(" · ")));

    QString rows;
    auto row = [&rows](const QString& label, const QString& value) {
        if (!value.isEmpty()) rows += QStringLiteral("<tr><td><i>%1</i>&nbsp;&nbsp;</td><td>%2</td></tr>").arg(label, value);
    };
    // The chain to the root: Mesh ‹ PointBased ‹ Gprim ...
    QStringList chain;
    for (const SchemaInfo* base = find(info.base); base; base = find(base->base)) chain << schemaLink(base->name);
    row(tr("Inherits"), chain.join(QStringLiteral(" &lsaquo; ")));
    row(tr("Built-in APIs"), links(info.builtInAPIs));
    row(tr("Auto-applied"), links(info.autoAppliedAPIs));
    if (isAPI(info.kind) && info.kind != SchemaInfo::Kind::NonAppliedAPI) {
        row(tr("Applies itself to"), links(info.autoApplyTo));
        row(tr("Can apply to"), info.canOnlyApplyTo.empty() ? tr("any prim") : links(info.canOnlyApplyTo));
    }
    if (!info.propertyNamespacePrefix.empty()) {
        row(tr("Instances"), tr("%1:<i>name</i>, properties under <code>%2:<i>name</i>:</code>")
                                 .arg(escaped(info.name), escaped(info.propertyNamespacePrefix)));
    }
    std::vector<std::string> derived, builtInto;
    for (const SchemaInfo& other : m_schemas) {
        if (other.base == info.name) derived.push_back(other.name);
        if (std::find(other.builtInAPIs.begin(), other.builtInAPIs.end(), info.name) != other.builtInAPIs.end()) {
            builtInto.push_back(other.name);
        }
    }
    row(tr("Derived"), links(derived));
    row(tr("Built into"), links(builtInto));
    QStringList files;
    if (!info.sourcePath.empty()) files << QStringLiteral("<a href=\"source:%1\">schema.usda</a>").arg(escaped(info.name));
    if (!info.generatedPath.empty()) {
        files << QStringLiteral("<a href=\"generated:%1\">generatedSchema.usda</a>").arg(escaped(info.name));
    }
    row(tr("Files"), files.join(QStringLiteral(", ")));
    if (!rows.isEmpty()) html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">%1</table>").arg(rows);
    html += info.documentation.empty() ? QStringLiteral("<p><i>%1</i></p>").arg(tr("No documentation."))
                                       : documentationHtml(info.documentation);
    return html;
}

void SchemaExplorerViewPanel::showDetails() {
    m_properties->clear();
    const SchemaInfo* info = currentSchema();
    if (!info) {
        m_details->setHtml(QStringLiteral("<p><i>%1</i></p>").arg(tr("Choose a schema to see what it defines.")));
        return;
    }
    m_details->setHtml(detailsHtml(*info));
    for (const SchemaProperty& property : info->properties) {
        auto* item = new QTreeWidgetItem(m_properties);
        item->setText(PropertyName, qs(property.name));
        item->setText(PropertyType, property.relationship ? tr("relationship")
                                                          : (property.uniform ? tr("uniform %1") : QStringLiteral("%1"))
                                                                .arg(qs(property.typeName)));
        item->setText(Fallback, qs(property.fallback));
        item->setText(DeclaredBy, qs(property.declaredBy));
        const QString tip = propertyToolTip(property);
        for (int column = 0; column < PropertyColumnCount; ++column) item->setToolTip(column, tip);
        if (property.declaredBy != info->name) {
            // Inherited or from a built-in API: the schema's own come first to the eye.
            item->setForeground(DeclaredBy, m_properties->palette().brush(QPalette::Disabled, QPalette::Text));
        }
    }
    for (int column = 0; column < DeclaredBy; ++column) m_properties->resizeColumnToContents(column);
    m_properties->setHeaderLabels(
        {tr("Property (%1)").arg(info->properties.size()), tr("Type"), tr("Fallback"), tr("Declared by")});
}

void SchemaExplorerViewPanel::openLink(const QUrl& url) {
    const QString target = url.toString();
    if (target.startsWith(QLatin1String("schema:"))) {
        selectSchema(target.mid(7));
    } else if (target.startsWith(QLatin1String("source:"))) {
        showSource(target.mid(7));
    } else if (target.startsWith(QLatin1String("generated:"))) {
        showSource(target.mid(10), true);
    }
}

UsdaPreviewDialog* SchemaExplorerViewPanel::showSource(const QString& name, bool generated) {
    const SchemaInfo* info = find(name.toStdString());
    if (!info) return nullptr;
    const std::string& path = generated || info->sourcePath.empty() ? info->generatedPath : info->sourcePath;
    if (path.empty()) return nullptr;
    UsdaPreviewDialog* dialog = showUsdaPreview(qs(path), this);
    if (dialog) dialog->view()->scrollToPrim(PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(info->name)));
    return dialog;
}

void SchemaExplorerViewPanel::showMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    const SchemaInfo* info = item ? find(item->data(Name, kNameRole).toString().toStdString()) : nullptr;
    if (!info) return;
    const QString name = qs(info->name);
    QMenu menu(this);
    QAction* source = menu.addAction(tr("Show Source"), this, [this, name]() { showSource(name); });
    source->setToolTip(tr("Its plugin's schema.usda, where the schema is written"));
    source->setEnabled(!info->sourcePath.empty() || !info->generatedPath.empty());
    QAction* generated = menu.addAction(tr("Show Generated Schema"), this, [this, name]() { showSource(name, true); });
    generated->setToolTip(tr("Its plugin's generatedSchema.usda, what USD reads"));
    generated->setEnabled(!info->generatedPath.empty());
    menu.setToolTipsVisible(true);
    menu.addSeparator();
    menu.addAction(tr("Copy Name"), this, [name]() { QApplication::clipboard()->setText(name); });
    const QString typeName = qs(info->typeName);
    menu.addAction(tr("Copy Type Name"), this, [typeName]() { QApplication::clipboard()->setText(typeName); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

}  // namespace usdcc::usd
