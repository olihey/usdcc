#include "usdcc/usd/shader_node_palette.h"

#include "usdcc/usd/shader_node_catalog.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace usdcc::usd {

namespace {
constexpr int kIdentifierRole = Qt::UserRole;
constexpr int kSearchTextRole = Qt::UserRole + 1;

// A tree whose nodes drag out as their identifier.
class PaletteTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(ShaderNodePalette::kMimeType)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        if (items.isEmpty()) return nullptr;
        const QString identifier = items.front()->data(0, kIdentifierRole).toString();
        if (identifier.isEmpty()) return nullptr;
        auto* data = new QMimeData;
        data->setData(QString::fromLatin1(ShaderNodePalette::kMimeType), identifier.toUtf8());
        return data;
    }
};

void addGroup(QTreeWidgetItem* parent, const ShaderNodeGroup& group) {
    auto* item = new QTreeWidgetItem(parent, {QString::fromStdString(group.name)});
    item->setFlags(Qt::ItemIsEnabled);
    for (const ShaderNodeGroup& child : group.groups) addGroup(item, child);
    for (const ShaderNodeInfo& node : group.nodes) {
        const QString label = QString::fromStdString(node.variant.empty() ? node.label : node.variant);
        auto* leaf = new QTreeWidgetItem(item, {label});
        leaf->setData(0, kIdentifierRole, QString::fromStdString(node.identifier));
        leaf->setData(0, kSearchTextRole, QString::fromStdString(node.label + " " + node.function + " " + node.identifier).toLower());
        leaf->setToolTip(0, QString::fromStdString(node.identifier + (node.help.empty() ? std::string() : "\n" + node.help)));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    }
}

void forEachLeaf(QTreeWidgetItem* item, const std::function<void(QTreeWidgetItem*)>& visit) {
    if (!item->data(0, kIdentifierRole).toString().isEmpty()) visit(item);
    for (int i = 0; i < item->childCount(); ++i) forEachLeaf(item->child(i), visit);
}

// Shows what matches and what leads to it. True if anything under it does.
bool filterItem(QTreeWidgetItem* item, const QStringList& words) {
    bool any = false;
    for (int i = 0; i < item->childCount(); ++i) any = filterItem(item->child(i), words) || any;
    const QString text = item->data(0, kSearchTextRole).toString();
    if (!text.isEmpty()) {
        any = std::all_of(words.begin(), words.end(), [&text](const QString& word) { return text.contains(word); });
    }
    item->setHidden(!any);
    if (any && !words.isEmpty()) item->setExpanded(true);
    return any;
}

QTreeWidgetItem* firstVisibleLeaf(QTreeWidgetItem* item) {
    if (item->isHidden()) return nullptr;
    if (!item->data(0, kIdentifierRole).toString().isEmpty()) return item;
    for (int i = 0; i < item->childCount(); ++i) {
        if (QTreeWidgetItem* found = firstVisibleLeaf(item->child(i))) return found;
    }
    return nullptr;
}
}  // namespace

ShaderNodePalette::ShaderNodePalette(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("shaderNodePalette"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search shader nodes"));
    m_search->setClearButtonEnabled(true);
    m_tree = new PaletteTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) { applySearch(text); });
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        const QString identifier = firstMatch();
        if (!identifier.isEmpty()) Q_EMIT nodeChosen(identifier);
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const QString identifier = item->data(0, kIdentifierRole).toString();
        if (!identifier.isEmpty()) Q_EMIT nodeChosen(identifier);
    });
    m_search->installEventFilter(this);
    refresh();
}

void ShaderNodePalette::setPreferredSystems(const std::vector<std::string>& systems) {
    if (systems == m_preferred) return;
    m_preferred = systems;
    refresh();
}

void ShaderNodePalette::refresh() {
    const ShaderNodeCatalog& catalog = ShaderNodeCatalog::instance();
    m_tree->clear();
    std::vector<ShaderNodeGroup> roots = catalog.tree();
    // By the name each is shown as, and those preferred first.
    std::vector<std::string> preferredNames;
    for (const std::string& system : m_preferred) {
        const RendererProfile* profile = catalog.profile(system);
        preferredNames.push_back(profile ? profile->displayName : system);
    }
    std::stable_sort(roots.begin(), roots.end(), [&preferredNames](const ShaderNodeGroup& a, const ShaderNodeGroup& b) {
        const bool aFirst = std::find(preferredNames.begin(), preferredNames.end(), a.name) != preferredNames.end();
        const bool bFirst = std::find(preferredNames.begin(), preferredNames.end(), b.name) != preferredNames.end();
        return aFirst && !bFirst;
    });
    for (const ShaderNodeGroup& root : roots) {
        auto* item = new QTreeWidgetItem(m_tree, {QString::fromStdString(root.name)});
        item->setFlags(Qt::ItemIsEnabled);
        for (const ShaderNodeGroup& group : root.groups) addGroup(item, group);
        const bool preferred = std::find(preferredNames.begin(), preferredNames.end(), root.name) != preferredNames.end();
        item->setExpanded(preferred);
    }
    applySearch(m_search->text());
}

void ShaderNodePalette::applySearch(const QString& text) {
    const QStringList words = text.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) filterItem(m_tree->topLevelItem(i), words);
}

QString ShaderNodePalette::firstMatch() const {
    // What's selected, then a label that starts with it, then whatever's first.
    if (const QTreeWidgetItem* current = m_tree->currentItem(); current && !current->isHidden() && !current->data(0, kIdentifierRole).toString().isEmpty()) {
        return current->data(0, kIdentifierRole).toString();
    }
    const QString word = m_search->text().trimmed().toLower();
    QString first;
    QString exact;
    QString starts;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        forEachLeaf(m_tree->topLevelItem(i), [&](QTreeWidgetItem* leaf) {
            if (leaf->isHidden()) return;
            const QString identifier = leaf->data(0, kIdentifierRole).toString();
            if (first.isEmpty()) first = identifier;
            const QString lower = identifier.toLower();
            if (exact.isEmpty() && lower == word) exact = identifier;
            if (starts.isEmpty() && !word.isEmpty() && lower.startsWith(word)) starts = identifier;
        });
    }
    return !exact.isEmpty() ? exact : !starts.isEmpty() ? starts : first;
}

bool ShaderNodePalette::eventFilter(QObject* watched, QEvent* event) {
    // Down from the search goes to its results.
    if (watched == m_search && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            if (QTreeWidgetItem* found = firstVisibleLeaf(m_tree->topLevelItem(i))) {
                m_tree->setCurrentItem(found);
                m_tree->setFocus();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

ShaderNodeSearchPopup::ShaderNodeSearchPopup(QWidget* parent) : QFrame(parent, Qt::Popup), m_palette(new ShaderNodePalette(this)) {
    setObjectName(QStringLiteral("shaderNodeSearchPopup"));
    setFrameShape(QFrame::StyledPanel);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_palette);
    resize(340, 420);
    connect(m_palette, &ShaderNodePalette::nodeChosen, this, [this](const QString& identifier) {
        Q_EMIT nodeChosen(identifier);
        close();
    });
}

}  // namespace usdcc::usd
