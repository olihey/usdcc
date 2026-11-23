#include "usdcc/usd/schema_explorer_view_panel.h"
#include "usdcc/ui/theme.h"

#include "usdcc/usd/apply_schema_ui.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/schema_libraries.h"
#include "usdcc/usd/schema_library_editor.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_view.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/property.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QResizeEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QSplitter>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
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
constexpr int kUnregisteredRole = Qt::UserRole + 1;  // This stage: a name USD doesn't know
constexpr int kStageRowRole = Qt::UserRole + 2;      // This stage: a schema it uses
constexpr int kLibraryRole = Qt::UserRole + 3;       // Schema Libraries: a library's name
// As the Applied Schemas pill of an unregistered schema (theme/app.qss).
QColor unregisteredColor() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Warning); }
constexpr std::size_t kMaxPrimsListed = 20;

enum Column { Name, Kind, ColumnCount };
enum PropertyColumn { PropertyName, PropertyType, Fallback, DeclaredBy, PropertyColumnCount };

QString qs(const std::string& text) { return QString::fromStdString(text); }

// "1 prim", "3 prims".
QString primCount(std::size_t n) {
    return n == 1 ? QObject::tr("1 prim") : QObject::tr("%1 prims").arg(n);
}

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
        case SchemaExplorerViewPanel::Show::OnStage: return true;  // the panel asks its counts
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

SchemaExplorerViewPanel::SchemaExplorerViewPanel(StageManager* stageManager, StageRefPtr stage,
                                                 std::shared_ptr<UndoManager> undoManager, const QString& title,
                                                 QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/app.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    m_filter = new QLineEdit(toolbar);
    m_filter->setObjectName(QStringLiteral("schemaFilter"));
    m_filter->setPlaceholderText(tr("Filter schemas"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMinimumWidth(fontMetrics().averageCharWidth() * 8);
    m_filter->setToolTip(tr("Show the schemas whose name has this in it"));
    toolbarLayout->addWidget(m_filter, 1);
    m_show = new QComboBox(toolbar);
    m_show->setObjectName(QStringLiteral("schemaShow"));
    m_show->addItem(tr("All"), int(Show::All));
    m_show->addItem(tr("Prim Types"), int(Show::PrimTypes));
    m_show->addItem(tr("Abstract Types"), int(Show::AbstractTypes));
    m_show->addItem(tr("API Schemas"), int(Show::APISchemas));
    m_show->addItem(tr("Codeless"), int(Show::Codeless));
    m_show->addItem(tr("On This Stage"), int(Show::OnStage));
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
    // A library opens in the editor.
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (const QString library = item ? item->data(Name, kLibraryRole).toString() : QString(); !library.isEmpty()) {
            QMetaObject::invokeMethod(this, [this, library]() { editLibrary(library); }, Qt::QueuedConnection);
        }
    });

    m_details = new QTextBrowser(container);
    m_details->setObjectName(QStringLiteral("schemaDetails"));
    m_details->setOpenLinks(false);
    m_details->setFrameShape(QFrame::NoFrame);
    // Queued: a link can create a prim or ask a question, which rebuilds the details.
    connect(m_details, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QMetaObject::invokeMethod(this, [this, url]() { openLink(url); }, Qt::QueuedConnection);
    });

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
    // The details, or the Schema Editor in their place.
    m_stack = new QStackedWidget(container);
    m_stack->addWidget(right);
    m_editor = new SchemaLibraryEditor(m_stack);
    m_stack->addWidget(m_editor);
    // Only the page showing counts towards the panel's size: the editor is wider than the
    // details, and a narrow panel (a workspace's side column) would scroll sideways for it.
    auto sizeToCurrent = [this](int current) {
        for (int i = 0; i < m_stack->count(); ++i) {
            const QSizePolicy::Policy policy = i == current ? QSizePolicy::Preferred : QSizePolicy::Ignored;
            m_stack->widget(i)->setSizePolicy(policy, policy);
        }
        m_stack->adjustSize();
    };
    connect(m_stack, &QStackedWidget::currentChanged, this, sizeToCurrent);
    sizeToCurrent(0);
    connect(m_editor, &SchemaLibraryEditor::closeRequested, this, [this]() {
        m_stack->setCurrentIndex(0);
        rebuildLibraryGroup();
    });
    connect(m_editor, &SchemaLibraryEditor::libraryChanged, this, [this]() { rebuildLibraryGroup(); });
    auto* split = new QSplitter(Qt::Horizontal, container);
    m_split = split;
    split->setChildrenCollapsible(false);
    split->addWidget(m_tree);
    split->addWidget(m_stack);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    layout->addWidget(toolbar);
    layout->addWidget(split, 1);
    setContentWidget(container);

    connect(m_filter, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(m_show, &QComboBox::currentIndexChanged, this, [this]() { applyFilter(); });

    // A single edit can send several notices, and a drag many.
    m_stageTimer = new QTimer(this);
    m_stageTimer->setSingleShot(true);
    m_stageTimer->setInterval(150);
    connect(m_stageTimer, &QTimer::timeout, this, &SchemaExplorerViewPanel::refreshStageUse);
    usdcc::ui::theme::onChanged(this, [this]() { refresh(); });
    // Hidden behind another tab, edits are only noted, and counted when it shows.
    m_tree->installEventFilter(this);
    // Create and Apply to Selection say what they'd do with the selection.
    m_selectionConnection =
        this->stageManager()->selectionChanged.connect([this](StageRefPtr stage, std::vector<PXR_NS::SdfPath>) {
            if (stage == currentStage()) updateDetailsText();
        });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

void SchemaExplorerViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    if (stage) {
        // Types and applied schemas change by resync.
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges& changes) {
            if (!changes.resyncedPaths.empty()) m_stageTimer->start();
        });
    }
    refresh();
}

void SchemaExplorerViewPanel::resizeEvent(QResizeEvent* event) {
    usdcc::ui::ViewPanel::resizeEvent(event);
    updateOrientation();
}

void SchemaExplorerViewPanel::updateOrientation() {
    // About 80 characters: below that, side by side leaves the tree too narrow to read
    // (a panel in a workspace's side column, say).
    const bool narrow = width() < fontMetrics().averageCharWidth() * 80;
    // The count goes first, so the toolbar fits too.
    m_summary->setVisible(!narrow);
    const Qt::Orientation orientation = narrow ? Qt::Vertical : Qt::Horizontal;
    if (m_split->orientation() == orientation) return;
    m_split->setOrientation(orientation);
    const int total = narrow ? m_split->height() : m_split->width();
    if (total > 0) m_split->setSizes({total * 2 / 5, total - total * 2 / 5});
}

bool SchemaExplorerViewPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tree && event->type() == QEvent::Show && m_stageUseStale) {
        QMetaObject::invokeMethod(this, &SchemaExplorerViewPanel::refreshStageUse, Qt::QueuedConnection);
    }
    return usdcc::ui::ViewPanel::eventFilter(watched, event);
}

usdcc::ui::ViewPanel* SchemaExplorerViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new SchemaExplorerViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
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

QString SchemaExplorerViewPanel::currentUnregistered() const {
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item ? item->data(Name, kUnregisteredRole).toString() : QString();
}

bool SchemaExplorerViewPanel::selectUnregistered(const QString& name) {
    if (!m_stageGroup) return false;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(Name, kUnregisteredRole).toString() != name) continue;
        if ((*it)->isHidden()) {
            const QSignalBlocker blocker(m_show);
            m_show->setCurrentIndex(0);
            m_filter->clear();
            applyFilter();
        }
        for (QTreeWidgetItem* parent = (*it)->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
        m_tree->setCurrentItem(*it);
        m_tree->scrollToItem(*it);
        return true;
    }
    return false;
}

std::vector<PXR_NS::SdfPath> SchemaExplorerViewPanel::selection() const {
    return currentStage() ? stageManager()->selectedPaths(currentStage()) : std::vector<PXR_NS::SdfPath>();
}

bool SchemaExplorerViewPanel::selectPrimsUsing(const QString& schema) {
    if (!currentStage()) return false;
    const std::string name = schema.toStdString();
    const std::vector<PXR_NS::SdfPath>* prims = nullptr;
    if (const auto it = m_use.prims.find(name); it != m_use.prims.end()) prims = &it->second;
    if (const auto it = m_use.unregistered.find(name); !prims && it != m_use.unregistered.end()) prims = &it->second;
    if (!prims || prims->empty()) return false;
    stageManager()->setSelectedPaths(currentStage(), *prims);
    return true;
}

QString SchemaExplorerViewPanel::createTargetText(bool* possible) const {
    if (possible) *possible = false;
    if (!currentStage()) return tr("No stage");
    const prim_actions::CreateTarget target = prim_actions::createTarget(currentStage(), selection());
    if (!target.problem.isEmpty()) return target.problem;
    if (possible) *possible = true;
    return target.parent == PXR_NS::SdfPath::AbsoluteRootPath() ? tr("at the root")
                                                                : tr("under %1").arg(qs(target.parent.GetString()));
}

PXR_NS::SdfPath SchemaExplorerViewPanel::createPrim(const QString& type, QString* why) {
    const SchemaInfo* info = find(type.toStdString());
    auto refuse = [why](const QString& reason) {
        if (why) *why = reason;
        return PXR_NS::SdfPath();
    };
    if (!info || info->kind != SchemaInfo::Kind::ConcreteTyped) return refuse(tr("%1 isn't a prim type").arg(type));
    if (!currentStage() || !m_undoManager) return refuse(tr("No stage"));
    const prim_actions::CreateTarget target = prim_actions::createTarget(currentStage(), selection());
    if (!target.problem.isEmpty()) return refuse(target.problem);
    const UndoManager::SelectionScope selected(*m_undoManager, currentStage());  // selecting it is part of the step
    const PXR_NS::SdfPath path = prim_actions::createPrim(*m_undoManager, currentStage(), target.parent, info->name);
    if (path.IsEmpty()) return refuse(tr("It couldn't be created there"));
    stageManager()->setSelectedPaths(currentStage(), {path});
    return path;
}

bool SchemaExplorerViewPanel::applyToSelection(const QString& schema) {
    const SchemaInfo* info = find(schema.toStdString());
    if (!info || !currentStage() || !m_undoManager) return false;
    if (info->kind != SchemaInfo::Kind::SingleApplyAPI && info->kind != SchemaInfo::Kind::MultipleApplyAPI) return false;
    const std::vector<PXR_NS::SdfPath> paths = selection();
    if (paths.empty()) return false;
    return askAndApplySchema(this, *m_undoManager, currentStage(), paths, qs(info->name),
                             info->kind == SchemaInfo::Kind::MultipleApplyAPI);
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
    menu->addSeparator();
    QAction* newLibraryAction = menu->addAction(tr("New Schema Library..."), this, [this]() {
        QMetaObject::invokeMethod(this, [this]() { newLibraryInteractively(); }, Qt::QueuedConnection);
    });
    newLibraryAction->setToolTip(tr("Make a library of codeless schemas, as a plugin in your plugins folder"));
}

bool SchemaExplorerViewPanel::canClose() { return !isEditing() || m_editor->maybeSave(); }

bool SchemaExplorerViewPanel::isEditing() const { return m_stack->currentIndex() == 1; }

bool SchemaExplorerViewPanel::editLibrary(const QString& library, const QString& schema) {
    const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(library.toStdString());
    if (!location) return false;
    if (isEditing() && m_editor->library() != library && !m_editor->maybeSave()) return false;
    if (!isEditing() || m_editor->library() != library) {
        QString error;
        if (!m_editor->open(library, &error)) {
            QMessageBox::warning(this, tr("Schema Library"), error);
            return false;
        }
    }
    if (!schema.isEmpty()) m_editor->selectClass(schema);
    m_stack->setCurrentIndex(1);
    return true;
}

bool SchemaExplorerViewPanel::closeEditor() {
    if (!isEditing()) return true;
    if (!m_editor->maybeSave()) return false;
    m_stack->setCurrentIndex(0);
    rebuildLibraryGroup();
    return true;
}

bool SchemaExplorerViewPanel::newLibrary(const QString& name, QString* error) {
    std::string why;
    SchemaLibraryLocation location;
    if (!SchemaLibraries::instance().create(name.toStdString(), &location, &why)) {
        if (error) *error = qs(why);
        return false;
    }
    rebuildLibraryGroup();
    return editLibrary(name);
}

void SchemaExplorerViewPanel::newLibraryInteractively() {
    QString name;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("New Schema Library"),
                                     tr("Name, as its schemas' namespace (studio, myShow):"), QLineEdit::Normal, name, &ok)
                   .trimmed();
        if (!ok || name.isEmpty()) return;
        QString error;
        if (newLibrary(name, &error)) return;
        QMessageBox::warning(this, tr("New Schema Library"), error);
    }
}

QString SchemaExplorerViewPanel::currentLibrary() const {
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item ? item->data(Name, kLibraryRole).toString() : QString();
}

bool SchemaExplorerViewPanel::selectLibrary(const QString& name) {
    for (int i = 0; m_libraryGroup && i < m_libraryGroup->childCount(); ++i) {
        if (m_libraryGroup->child(i)->data(Name, kLibraryRole).toString() != name) continue;
        m_libraryGroup->setExpanded(true);
        m_tree->setCurrentItem(m_libraryGroup->child(i));
        return true;
    }
    return false;
}

void SchemaExplorerViewPanel::rebuildLibraryGroup() {
    const QString chosen = currentLibrary();
    const bool wasExpanded = m_libraryGroup ? m_libraryGroup->isExpanded() : true;
    delete m_libraryGroup;
    m_libraryGroup = nullptr;
    const std::vector<SchemaLibraryLocation> libraries = SchemaLibraries::instance().libraries();
    if (libraries.empty()) return;
    // Before This stage, after the categories.
    m_libraryGroup = new QTreeWidgetItem();
    const int at = m_stageGroup ? m_tree->indexOfTopLevelItem(m_stageGroup) : m_tree->topLevelItemCount();
    m_tree->insertTopLevelItem(at, m_libraryGroup);
    m_libraryGroup->setText(Name, tr("Schema Libraries"));
    QFont font = m_libraryGroup->font(Name);
    font.setBold(true);
    m_libraryGroup->setFont(Name, font);
    m_libraryGroup->setFlags(m_libraryGroup->flags() & ~Qt::ItemIsSelectable);
    m_libraryGroup->setToolTip(Name, tr("Libraries of codeless schemas usdcc can edit. Double-click one to edit it"));
    for (const SchemaLibraryLocation& library : libraries) {
        const QString state = !library.writable           ? tr("read-only")
                              : !library.generated         ? tr("not generated")
                              : library.changed            ? tr("changed")
                              : library.waitingForRestart  ? tr("restart to load")
                                                           : tr("loaded");
        auto* item = new QTreeWidgetItem(m_libraryGroup, {qs(library.name), state});
        item->setData(Name, kLibraryRole, qs(library.name));
        item->setToolTip(Name, qs(library.source()));
    }
    m_libraryGroup->setExpanded(wasExpanded);
    if (!chosen.isEmpty()) {
        const QSignalBlocker blocker(m_tree);
        selectLibrary(chosen);
    }
}

QString SchemaExplorerViewPanel::libraryHtml(const std::string& name) const {
    const std::optional<SchemaLibraryLocation> library = SchemaLibraries::instance().find(name);
    if (!library) return QStringLiteral("<p><i>%1</i></p>").arg(tr("It's gone."));
    QString html = QStringLiteral("<h3>%1</h3>").arg(escaped(name));
    const QString state = !library->writable           ? tr("Read-only: its folder can't be written.")
                          : !library->generated         ? tr("Not generated yet: USD doesn't know its schemas.")
                          : library->changed            ? tr("Changed since it was generated.")
                          : library->waitingForRestart  ? tr("Generated: it loads at the next start.")
                                                        : tr("Loaded.");
    html += QStringLiteral("<p>%1 <a href=\"edit:%2\">%3</a></p>").arg(state, escaped(name), tr("Edit"));
    QString rows;
    auto row = [&rows](const QString& label, const QString& value) {
        rows += QStringLiteral("<tr><td><i>%1</i>&nbsp;&nbsp;</td><td>%2</td></tr>").arg(label, value);
    };
    row(tr("Source"), escaped(library->source()));
    if (!library->pluginFolder.empty()) row(tr("Plugin"), escaped(library->pluginFolder));
    SchemaLibraryDef definition;
    if (readSchemaLibrary(library->source(), &definition, nullptr)) {
        QStringList schemas;
        for (const SchemaClassDef& c : definition.classes) {
            schemas << (find(c.name) ? schemaLink(c.name) : escaped(c.name));
        }
        row(tr("Schemas"), schemas.isEmpty() ? tr("none yet") : schemas.join(QStringLiteral(", ")));
    }
    html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">%1</table>").arg(rows);
    return html;
}

std::vector<PXR_NS::SdfPath> SchemaExplorerViewPanel::primsWithAttributesOf(const QString& schema) const {
    std::vector<PXR_NS::SdfPath> prims;
    const SchemaInfo* info = find(schema.toStdString());
    if (!info || info->kind != SchemaInfo::Kind::SingleApplyAPI || !currentStage()) return prims;
    std::vector<PXR_NS::TfToken> names;
    for (const SchemaProperty& property : info->properties) names.emplace_back(property.name);
    const PXR_NS::TfToken api(info->name);
    for (const PXR_NS::UsdPrim& prim : currentStage()->usdStage()->Traverse()) {
        const PXR_NS::TfTokenVector applied = prim.GetAppliedSchemas();
        if (std::find(applied.begin(), applied.end(), api) != applied.end()) continue;
        for (const PXR_NS::TfToken& name : names) {
            const PXR_NS::UsdProperty property = prim.GetProperty(name);
            if (property && property.IsAuthored()) {
                prims.push_back(prim.GetPath());
                break;
            }
        }
    }
    return prims;
}

bool SchemaExplorerViewPanel::applyToPrimsWithItsAttributes(const QString& schema) {
    const std::vector<PXR_NS::SdfPath> prims = primsWithAttributesOf(schema);
    if (prims.empty() || !m_undoManager) return false;
    return prim_actions::applySchema(*m_undoManager, currentStage(), prims, schema.toStdString());
}

void SchemaExplorerViewPanel::refresh() {
    const QString current = currentSchema() ? qs(currentSchema()->name) : QString();
    const QString unregistered = currentUnregistered();
    m_schemas = allSchemas();
    m_index.clear();
    for (std::size_t i = 0; i < m_schemas.size(); ++i) m_index[m_schemas[i].name] = i;
    m_stageTimer->stop();
    m_stageUseStale = false;
    m_use = currentStage() ? stageSchemaUse(currentStage()->usdStage()) : StageSchemaUse();
    {
        const QSignalBlocker blocker(m_tree);  // one showDetails() below, not one per step
        m_stageGroup = nullptr;                // rebuildTree() clears the tree, the group too
        m_libraryGroup = nullptr;
        const QString library = currentLibrary();
        rebuildTree();
        rebuildStageGroup();
        rebuildLibraryGroup();
        applyFilter();
        const bool found = !unregistered.isEmpty() ? selectUnregistered(unregistered)
                           : !library.isEmpty()    ? selectLibrary(library)
                                                   : !current.isEmpty() && selectSchema(current);
        if (!found) m_tree->setCurrentItem(nullptr);
    }
    showDetails();
}

void SchemaExplorerViewPanel::refreshStageUse() {
    m_stageTimer->stop();
    if (!m_tree->isVisible()) {
        m_stageUseStale = true;
        return;
    }
    m_stageUseStale = false;
    // What's chosen in This stage, to choose again in the new one.
    const QTreeWidgetItem* item = m_tree->currentItem();
    const QString unregistered = currentUnregistered();
    const QString stageRow =
        item && item->data(Name, kStageRowRole).toBool() ? item->data(Name, kNameRole).toString() : QString();
    m_use = currentStage() ? stageSchemaUse(currentStage()->usdStage()) : StageSchemaUse();
    {
        const QSignalBlocker blocker(m_tree);
        rebuildStageGroup();
        applyFilter();
        if (!unregistered.isEmpty() && !selectUnregistered(unregistered)) m_tree->setCurrentItem(nullptr);
        if (!stageRow.isEmpty()) {
            m_tree->setCurrentItem(nullptr);
            for (int i = 0; m_stageGroup && i < m_stageGroup->childCount(); ++i) {
                if (m_stageGroup->child(i)->data(Name, kNameRole).toString() == stageRow) {
                    m_tree->setCurrentItem(m_stageGroup->child(i));
                }
            }
        }
    }
    if (!unregistered.isEmpty() || !stageRow.isEmpty()) {
        showDetails();  // the row is a new one
    } else {
        updateDetailsText();  // the counts
    }
}

void SchemaExplorerViewPanel::rebuildStageGroup() {
    const bool hadGroup = m_stageGroup != nullptr;
    const bool wasExpanded = hadGroup && m_stageGroup->isExpanded();
    delete m_stageGroup;
    m_stageGroup = nullptr;
    if (!currentStage()) return;

    m_stageGroup = new QTreeWidgetItem(m_tree, {tr("This stage")});
    QFont font = m_stageGroup->font(Name);
    font.setBold(true);
    m_stageGroup->setFont(Name, font);
    m_stageGroup->setFlags(m_stageGroup->flags() & ~Qt::ItemIsSelectable);
    m_stageGroup->setText(Kind, m_use.prims.size() == 1 ? tr("1 schema") : tr("%1 schemas").arg(m_use.prims.size()));
    m_stageGroup->setToolTip(Name, tr("The types and API schemas the stage's prims have, inside instances and inactive "
                                      "prims too, and those it names that USD doesn't know"));
    for (const auto& [name, prims] : m_use.prims) {
        const SchemaInfo* info = find(name);
        if (!info) continue;
        auto* item = new QTreeWidgetItem(m_stageGroup, {qs(name), primCount(prims.size())});
        item->setData(Name, kNameRole, qs(name));
        item->setData(Name, kStageRowRole, true);
        if (isTyped(info->kind)) item->setIcon(Name, prim_icons::icon(prim_icons::iconNameForType(PXR_NS::TfToken(name))));
        item->setToolTip(Name, tr("%1, on %2").arg(qs(schemaKindName(info->kind)), primCount(prims.size())));
    }
    if (!m_use.unregistered.empty()) {
        auto* group = new QTreeWidgetItem(m_stageGroup, {tr("Not registered")});
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        group->setForeground(Name, unregisteredColor());
        group->setText(Kind, QString::number(m_use.unregistered.size()));
        group->setToolTip(Name, tr("Types and API schemas the stage names that USD doesn't know: their plugin may not be "
                                   "loaded, or the name is misspelt"));
        for (const auto& [name, prims] : m_use.unregistered) {
            auto* item = new QTreeWidgetItem(group, {qs(name), primCount(prims.size())});
            item->setData(Name, kUnregisteredRole, qs(name));
            item->setForeground(Name, unregisteredColor());
            item->setToolTip(Name, tr("Not registered: its plugin may not be loaded, or the name is misspelt"));
        }
        group->setExpanded(true);
    }
    // Open at first when there's something wrong to see.
    m_stageGroup->setExpanded(hadGroup ? wasExpanded : !m_use.unregistered.empty());
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
            const bool kind = show == Show::OnStage ? m_use.count(info->name) > 0 : matchesShow(*info, show);
            matches = kind && (text.isEmpty() || qs(info->name).contains(text, Qt::CaseInsensitive) ||
                               qs(info->typeName).contains(text, Qt::CaseInsensitive));
            // This stage's rows repeat the tree's: count each schema once.
            if (matches && !item->data(Name, kStageRowRole).toBool()) ++shown;
        } else if (const QString unregistered = item->data(Name, kUnregisteredRole).toString(); !unregistered.isEmpty()) {
            matches = (show == Show::All || show == Show::OnStage) &&
                      (text.isEmpty() || unregistered.contains(text, Qt::CaseInsensitive));
        } else if (const QString library = item->data(Name, kLibraryRole).toString(); !library.isEmpty()) {
            matches = (show == Show::All || show == Show::Codeless) &&
                      (text.isEmpty() || library.contains(text, Qt::CaseInsensitive));
        }
        item->setHidden(!matches && !any);
        // An ancestor shown only for a match below it is dimmed.
        // This stage's unregistered rows, and their group, keep their amber.
        const bool amber = item->data(Name, kUnregisteredRole).isValid() || item->data(Name, kLibraryRole).isValid() ||
                           (m_stageGroup && item->parent() == m_stageGroup && !item->data(Name, kNameRole).isValid());
        if (!amber) {
            item->setForeground(Name, matches || !item->data(Name, kNameRole).isValid() || !filtering
                                          ? QBrush()
                                          : m_tree->palette().brush(QPalette::Disabled, QPalette::Text));
        }
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
    // What the stage does with it, and what can be done.
    if (currentStage()) {
        row(tr("On this stage"), stageUseHtml(info.name));
        if (info.kind == SchemaInfo::Kind::ConcreteTyped) {
            bool possible = false;
            const QString where = createTargetText(&possible).toHtmlEscaped();
            row(tr("Create"), possible ? QStringLiteral("<a href=\"create:%1\">%2</a> %3").arg(escaped(info.name), tr("Create"), where)
                                       : QStringLiteral("<i>%1</i>").arg(where));
        } else if (info.kind == SchemaInfo::Kind::SingleApplyAPI || info.kind == SchemaInfo::Kind::MultipleApplyAPI) {
            const std::size_t selected = selection().size();
            row(tr("Apply"), selected == 0 ? QStringLiteral("<i>%1</i>").arg(tr("Select prims to apply it to"))
                                           : QStringLiteral("<a href=\"apply:%1\">%2</a> %3")
                                                 .arg(escaped(info.name), tr("Apply to Selection"),
                                                      tr("(%1 selected)").arg(primCount(selected))));
            // Prims that have its attributes, made into it by Promote to Schema (§6).
            if (info.codeless && info.kind == SchemaInfo::Kind::SingleApplyAPI) {
                const std::size_t having = primsWithAttributesOf(qs(info.name)).size();
                if (having > 0) {
                    row(tr("Has its attributes"), QStringLiteral("%1 · <a href=\"adopt:%2\">%3</a>")
                                                      .arg(primCount(having), escaped(info.name), tr("Apply to Them")));
                }
            }
        }
    }
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
    if (info.codeless) {
        const std::optional<SchemaLibraryLocation> library = SchemaLibraries::instance().find(info.plugin);
        if (library && library->writable) {
            row(tr("Library"), QStringLiteral("%1 · <a href=\"edit:%1:%2\">%3</a>").arg(escaped(info.plugin), escaped(info.name), tr("Edit")));
        }
    }
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

QString SchemaExplorerViewPanel::stageUseHtml(const std::string& name) const {
    std::size_t count = m_use.count(name);
    if (count == 0) {
        const auto it = m_use.unregistered.find(name);
        count = it == m_use.unregistered.end() ? 0 : it->second.size();
    }
    if (count == 0) return tr("none");
    return QStringLiteral("%1 · <a href=\"select:%2\">%3</a>").arg(primCount(count), escaped(name), tr("Select"));
}

QString SchemaExplorerViewPanel::unregisteredHtml(const std::string& name) const {
    QString html = QStringLiteral("<h3>%1</h3>").arg(escaped(name));
    html += QStringLiteral("<p>%1</p>").arg(tr("Not registered: USD knows no schema of this name. Its plugin may not be "
                                               "loaded, or the name is misspelt."));
    const auto composes = m_use.composesAs.find(name);
    if (composes != m_use.composesAs.end()) {
        // A type: what its prims are instead.
        html += QStringLiteral("<p>%1</p>")
                    .arg(composes->second.empty()
                             ? tr("Its prims have no schema: no properties of their own and no fallbacks. Their values "
                                  "are kept, as authored.")
                             : tr("Its prims compose as %1, which the stage's fallbackPrimTypes names for it. Their "
                                  "values are kept, as authored.")
                                   .arg(schemaLink(composes->second)));
    } else {
        html += QStringLiteral("<p>%1</p>").arg(tr("Applied in the file, it adds nothing: no properties and no "
                                                   "fallbacks. The Attribute Editor shows it as an amber pill."));
    }
    QString rows = QStringLiteral("<tr><td><i>%1</i>&nbsp;&nbsp;</td><td>%2</td></tr>")
                       .arg(tr("On this stage"), stageUseHtml(name));
    html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">%1</table>").arg(rows);
    if (const auto it = m_use.unregistered.find(name); it != m_use.unregistered.end()) {
        QStringList prims;
        for (std::size_t i = 0; i < it->second.size() && i < kMaxPrimsListed; ++i) {
            const QString path = qs(it->second[i].GetString());
            prims << QStringLiteral("<a href=\"prim:%1\">%2</a>").arg(path.toHtmlEscaped(), path.toHtmlEscaped());
        }
        if (it->second.size() > kMaxPrimsListed) prims << tr("and %1 more").arg(it->second.size() - kMaxPrimsListed);
        html += QStringLiteral("<p>%1</p>").arg(prims.join(QStringLiteral("<br>")));
    }
    return html;
}

void SchemaExplorerViewPanel::updateDetailsText() {
    // Kept where it was: the counts or the selection changed, not the schema.
    const int scroll = m_details->verticalScrollBar()->value();
    if (const QString unregistered = currentUnregistered(); !unregistered.isEmpty()) {
        m_details->setHtml(unregisteredHtml(unregistered.toStdString()));
    } else if (const QString library = currentLibrary(); !library.isEmpty()) {
        m_details->setHtml(libraryHtml(library.toStdString()));
    } else if (const SchemaInfo* info = currentSchema()) {
        m_details->setHtml(detailsHtml(*info));
    } else {
        m_details->setHtml(QStringLiteral("<p><i>%1</i></p>").arg(tr("Choose a schema to see what it defines.")));
    }
    m_details->verticalScrollBar()->setValue(scroll);
}

void SchemaExplorerViewPanel::showDetails() {
    m_properties->clear();
    m_properties->setHeaderLabels({tr("Property"), tr("Type"), tr("Fallback"), tr("Declared by")});
    updateDetailsText();
    m_details->verticalScrollBar()->setValue(0);
    const SchemaInfo* info = currentSchema();
    if (!info) return;
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
    const QString scheme = target.section(QLatin1Char(':'), 0, 0);
    const QString rest = target.section(QLatin1Char(':'), 1);
    if (scheme == QLatin1String("schema")) {
        selectSchema(rest);
    } else if (scheme == QLatin1String("source")) {
        showSource(rest);
    } else if (scheme == QLatin1String("generated")) {
        showSource(rest, true);
    } else if (scheme == QLatin1String("select")) {
        selectPrimsUsing(rest);
    } else if (scheme == QLatin1String("create")) {
        createPrim(rest);
    } else if (scheme == QLatin1String("apply")) {
        applyToSelection(rest);
    } else if (scheme == QLatin1String("edit")) {
        editLibrary(rest.section(QLatin1Char(':'), 0, 0), rest.section(QLatin1Char(':'), 1));
    } else if (scheme == QLatin1String("adopt")) {
        applyToPrimsWithItsAttributes(rest);
    } else if (scheme == QLatin1String("prim") && currentStage()) {
        stageManager()->setSelectedPaths(currentStage(), {PXR_NS::SdfPath(rest.toStdString())});
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
    if (!item) return;
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    auto addSelect = [this, &menu](const QString& name, std::size_t count) {
        QAction* select = menu.addAction(count ? tr("Select Prims Using It (%1)").arg(count) : tr("Select Prims Using It"),
                                         this, [this, name]() { selectPrimsUsing(name); });
        select->setEnabled(count > 0 && currentStage());
    };

    if (const QString library = item->data(Name, kLibraryRole).toString(); !library.isEmpty()) {
        menu.addAction(tr("Edit"), this, [this, library]() { editLibrary(library); });
        menu.addSeparator();
        const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(library.toStdString());
        const QString path = location ? qs(location->source()) : QString();
        QAction* source = menu.addAction(tr("Show Source"), this, [this, path]() { showUsdaPreview(path, this); });
        source->setEnabled(!path.isEmpty());
        menu.addAction(tr("Copy Path"), this, [path]() { QApplication::clipboard()->setText(path); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }
    if (const QString unregistered = item->data(Name, kUnregisteredRole).toString(); !unregistered.isEmpty()) {
        const auto it = m_use.unregistered.find(unregistered.toStdString());
        addSelect(unregistered, it == m_use.unregistered.end() ? 0 : it->second.size());
        menu.addSeparator();
        menu.addAction(tr("Copy Name"), this, [unregistered]() { QApplication::clipboard()->setText(unregistered); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }

    const SchemaInfo* info = find(item->data(Name, kNameRole).toString().toStdString());
    if (!info) return;
    const QString name = qs(info->name);
    if (currentStage()) {
        addSelect(name, m_use.count(info->name));
        if (info->kind == SchemaInfo::Kind::ConcreteTyped) {
            bool possible = false;
            const QString where = createTargetText(&possible);
            QAction* create = menu.addAction(tr("Create %1").arg(name), this, [this, name]() { createPrim(name); });
            create->setEnabled(possible);
            create->setToolTip(where);
        } else if (info->kind == SchemaInfo::Kind::SingleApplyAPI || info->kind == SchemaInfo::Kind::MultipleApplyAPI) {
            const std::size_t selected = selection().size();
            QAction* apply = menu.addAction(info->kind == SchemaInfo::Kind::MultipleApplyAPI ? tr("Apply to Selection...")
                                                                                            : tr("Apply to Selection"),
                                            this, [this, name]() {
                                                // Queued: it may ask, and the menu should be gone first.
                                                QMetaObject::invokeMethod(
                                                    this, [this, name]() { applyToSelection(name); }, Qt::QueuedConnection);
                                            });
            apply->setEnabled(selected > 0);
            apply->setToolTip(selected ? primCount(selected) : tr("Select prims to apply it to"));
        }
        menu.addSeparator();
    }
    QAction* source = menu.addAction(tr("Show Source"), this, [this, name]() { showSource(name); });
    source->setToolTip(tr("Its plugin's schema.usda, where the schema is written"));
    source->setEnabled(!info->sourcePath.empty() || !info->generatedPath.empty());
    QAction* generated = menu.addAction(tr("Show Generated Schema"), this, [this, name]() { showSource(name, true); });
    generated->setToolTip(tr("Its plugin's generatedSchema.usda, what USD reads"));
    generated->setEnabled(!info->generatedPath.empty());
    menu.addSeparator();
    menu.addAction(tr("Copy Name"), this, [name]() { QApplication::clipboard()->setText(name); });
    const QString typeName = qs(info->typeName);
    menu.addAction(tr("Copy Type Name"), this, [typeName]() { QApplication::clipboard()->setText(typeName); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

}  // namespace usdcc::usd
