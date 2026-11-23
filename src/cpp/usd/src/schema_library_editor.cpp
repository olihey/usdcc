#include "usdcc/usd/schema_library_editor.h"
#include "usdcc/ui/theme.h"

#include "usdcc/usd/schema_info.h"

#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QDesktopServices>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <set>

namespace usdcc::usd {

namespace {

SchemaEditorHooks& hooks() {
    static SchemaEditorHooks instance;
    return instance;
}

QString qs(const std::string& text) { return QString::fromStdString(text); }

// A file as it is now: its time and size.
QString stampOf(const QString& path) {
    const QFileInfo info(path);
    return info.exists() ? QStringLiteral("%1/%2").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()) : QString();
}

enum PropertyColumn { Name, Type, Uniform, Fallback, Tokens, DisplayName, DisplayGroup, Documentation, ColumnCount };

QColor errorColor() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Error); }  // as the log's errors
QColor warningColor() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Warning); }  // as an unregistered schema's pill
constexpr auto kRelationship = "relationship";

QString joined(const std::vector<std::string>& items) {
    QStringList list;
    for (const std::string& item : items) list << qs(item);
    return list.join(QStringLiteral(", "));
}

std::vector<std::string> split(const QString& text) {
    std::vector<std::string> items;
    for (const QString& item : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (const QString trimmed = item.trimmed(); !trimmed.isEmpty()) items.push_back(trimmed.toStdString());
    }
    return items;
}

// The value types a property can have, as USDA spells them (not the legacy aliases).
QStringList valueTypes() {
    static const QStringList types = []() {
        std::set<std::string> names;
        for (const PXR_NS::SdfValueTypeName& type : PXR_NS::SdfSchema::GetInstance().GetAllTypes()) {
            const std::string name = type.GetAsToken().GetString();
            if (!name.empty() && std::islower(static_cast<unsigned char>(name[0]))) names.insert(name);
        }
        QStringList list;
        for (const std::string& name : names) list << qs(name);
        return list;
    }();
    return types;
}

QString kindLabel(SchemaInfo::Kind kind) {
    switch (kind) {
        case SchemaInfo::Kind::ConcreteTyped: return QObject::tr("Prim Type");
        case SchemaInfo::Kind::AbstractTyped: return QObject::tr("Abstract Type");
        case SchemaInfo::Kind::SingleApplyAPI: return QObject::tr("API");
        case SchemaInfo::Kind::MultipleApplyAPI: return QObject::tr("Multiple-apply API");
        default: return QObject::tr("Non-applied API");
    }
}

bool isApiKind(SchemaInfo::Kind kind) { return kind != SchemaInfo::Kind::ConcreteTyped && kind != SchemaInfo::Kind::AbstractTyped; }

}  // namespace

void setSchemaEditorHooks(SchemaEditorHooks value) { hooks() = std::move(value); }
const SchemaEditorHooks& schemaEditorHooks() { return hooks(); }

SchemaLibraryEditor::SchemaLibraryEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("schemaLibraryEditor"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    // The library, its state, and what can be done with it.
    auto* top = new QHBoxLayout;
    m_title = new QLabel(this);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    top->addWidget(m_title);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("schemaLibraryStatus"));
    top->addWidget(m_status, 1);
    m_save = new QPushButton(tr("Save"), this);
    m_save->setToolTip(tr("Write schema.usda"));
    connect(m_save, &QPushButton::clicked, this, [this]() {
        QString error;
        if (!save(&error)) QMessageBox::warning(this, tr("Save"), error);
    });
    top->addWidget(m_save);
    m_generate = new QPushButton(tr("Generate"), this);
    m_generate->setToolTip(tr("Save, then make the library's plugin files with usdGenSchema. They load at the next start"));
    connect(m_generate, &QPushButton::clicked, this, [this]() { generate(true); });
    top->addWidget(m_generate);
    m_restart = new QPushButton(tr("Restart"), this);
    m_restart->setToolTip(tr("Close usdcc and start it again with this session, so the generated schemas load"));
    m_restart->setVisible(false);
    connect(m_restart, &QPushButton::clicked, this, [this]() {
        if (!maybeSave()) return;
        if (hooks().restart) hooks().restart();
    });
    top->addWidget(m_restart);
    auto* more = new QToolButton(this);
    more->setText(tr("More"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    moreMenu->addAction(tr("Edit as Text"), this, [this]() {
        if (m_modified) {
            QString error;
            if (!save(&error)) return;
        }
        const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(m_library.toStdString());
        if (!location) return;
        const QString path = qs(location->source());
        if (hooks().editText) {
            hooks().editText(path);
        } else {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });
    moreMenu->addAction(tr("Reload"), this, [this]() {
        if (m_modified && QMessageBox::question(this, tr("Reload"), tr("Drop the edits not saved?")) != QMessageBox::Yes) return;
        QString error;
        if (!reload(&error)) QMessageBox::warning(this, tr("Reload"), error);
    });
    moreMenu->addSeparator();
    moreMenu->addAction(tr("Close"), this, [this]() {
        if (maybeSave()) Q_EMIT closeRequested();
    });
    more->setMenu(moreMenu);
    top->addWidget(more);
    layout->addLayout(top);

    // Classes | the chosen one.
    auto* split = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(split);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_classes = new QListWidget(left);
    m_classes->setObjectName(QStringLiteral("schemaClasses"));
    connect(m_classes, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_updating) return;
        m_current = row;
        showClass();
    });
    leftLayout->addWidget(m_classes, 1);
    auto* classButtons = new QHBoxLayout;
    m_addClass = new QToolButton(left);
    m_addClass->setText(tr("+ Schema"));
    m_addClass->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(m_addClass);
    for (SchemaInfo::Kind kind : {SchemaInfo::Kind::ConcreteTyped, SchemaInfo::Kind::AbstractTyped,
                                  SchemaInfo::Kind::SingleApplyAPI, SchemaInfo::Kind::MultipleApplyAPI}) {
        addMenu->addAction(kindLabel(kind), this, [this, kind]() { addClass(kind); });
    }
    m_addClass->setMenu(addMenu);
    classButtons->addWidget(m_addClass);
    m_removeClass = new QToolButton(left);
    m_removeClass->setText(tr("Remove"));
    connect(m_removeClass, &QToolButton::clicked, this, [this]() { removeCurrentClass(); });
    classButtons->addWidget(m_removeClass);
    classButtons->addStretch(1);
    leftLayout->addLayout(classButtons);

    m_form = new QWidget(split);
    buildClassForm();
    split->addWidget(left);
    split->addWidget(m_form);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 4);
    layout->addWidget(split, 1);

    // What the checks say, and usdGenSchema.
    m_issues = new QListWidget(this);
    m_issues->setObjectName(QStringLiteral("schemaIssues"));
    m_issues->setMaximumHeight(110);
    m_issues->setWordWrap(true);
    connect(m_issues, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const QString schema = item->data(Qt::UserRole).toString();
        if (!schema.isEmpty()) selectClass(schema);
    });
    layout->addWidget(m_issues);

    m_checkTimer = new QTimer(this);
    m_checkTimer->setSingleShot(true);
    m_checkTimer->setInterval(250);
    connect(m_checkTimer, &QTimer::timeout, this, &SchemaLibraryEditor::refreshIssues);

    // Edit as Text: the form follows the file, unless it has edits of its own.
    auto* watcher = new QFileSystemWatcher(this);
    connect(this, &SchemaLibraryEditor::libraryChanged, watcher, [this, watcher]() {
        if (!watcher->files().isEmpty()) watcher->removePaths(watcher->files());
        if (const auto location = SchemaLibraries::instance().find(m_library.toStdString())) watcher->addPath(qs(location->source()));
    });
    connect(watcher, &QFileSystemWatcher::fileChanged, this, [this, watcher](const QString& path) {
        if (!watcher->files().contains(path)) watcher->addPath(path);  // replaced, not written: watch it again
        if (stampOf(path) == m_known) return;  // its own Save
        if (m_modified) {
            m_status->setText(tr("schema.usda changed on disk: More > Reload shows it"));
            return;
        }
        reload();
    });
}

SchemaLibraryEditor::~SchemaLibraryEditor() = default;

void SchemaLibraryEditor::buildClassForm() {
    auto* layout = new QVBoxLayout(m_form);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(m_form);
    m_name->setObjectName(QStringLiteral("schemaName"));
    connect(m_name, &QLineEdit::editingFinished, this, &SchemaLibraryEditor::classEdited);
    form->addRow(tr("Name"), m_name);
    m_kind = new QComboBox(m_form);
    for (SchemaInfo::Kind kind : {SchemaInfo::Kind::ConcreteTyped, SchemaInfo::Kind::AbstractTyped,
                                  SchemaInfo::Kind::SingleApplyAPI, SchemaInfo::Kind::MultipleApplyAPI}) {
        m_kind->addItem(kindLabel(kind), int(kind));
    }
    connect(m_kind, &QComboBox::currentIndexChanged, this, &SchemaLibraryEditor::classEdited);
    form->addRow(tr("Kind"), m_kind);
    m_base = new QComboBox(m_form);
    m_base->setEditable(true);
    m_base->setToolTip(tr("The prim type it inherits: USD's, or one of this library's"));
    connect(m_base, &QComboBox::currentTextChanged, this, &SchemaLibraryEditor::classEdited);
    m_baseLabel = new QLabel(tr("Inherits"), m_form);
    form->addRow(m_baseLabel, m_base);
    m_documentation = new QPlainTextEdit(m_form);
    m_documentation->setMaximumHeight(70);
    connect(m_documentation, &QPlainTextEdit::textChanged, this, &SchemaLibraryEditor::classEdited);
    form->addRow(tr("Documentation"), m_documentation);
    m_builtIn = new QLineEdit(m_form);
    m_builtIn->setPlaceholderText(tr("API schemas it always has, separated by commas"));
    connect(m_builtIn, &QLineEdit::editingFinished, this, &SchemaLibraryEditor::classEdited);
    form->addRow(tr("Built-in APIs"), m_builtIn);
    m_autoApply = new QLineEdit(m_form);
    m_autoApply->setPlaceholderText(tr("Prim types it applies itself to"));
    connect(m_autoApply, &QLineEdit::editingFinished, this, &SchemaLibraryEditor::classEdited);
    m_autoApplyLabel = new QLabel(tr("Applies itself to"), m_form);
    form->addRow(m_autoApplyLabel, m_autoApply);
    m_canApply = new QLineEdit(m_form);
    m_canApply->setPlaceholderText(tr("Empty: any prim"));
    connect(m_canApply, &QLineEdit::editingFinished, this, &SchemaLibraryEditor::classEdited);
    m_canApplyLabel = new QLabel(tr("Can only apply to"), m_form);
    form->addRow(m_canApplyLabel, m_canApply);
    m_prefix = new QLineEdit(m_form);
    m_prefix->setPlaceholderText(tr("Its properties go under prefix:name:"));
    connect(m_prefix, &QLineEdit::editingFinished, this, &SchemaLibraryEditor::classEdited);
    m_prefixLabel = new QLabel(tr("Property prefix"), m_form);
    form->addRow(m_prefixLabel, m_prefix);
    layout->addLayout(form);

    m_properties = new QTableWidget(0, ColumnCount, m_form);
    m_properties->setObjectName(QStringLiteral("schemaPropertyTable"));
    m_properties->setHorizontalHeaderLabels({tr("Property"), tr("Type"), tr("Uniform"), tr("Fallback"), tr("Allowed Tokens"),
                                             tr("Display Name"), tr("Display Group"), tr("Documentation")});
    m_properties->horizontalHeader()->setStretchLastSection(true);
    m_properties->verticalHeader()->setVisible(false);
    connect(m_properties, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!m_updating) propertyEdited(item->row());
    });
    layout->addWidget(m_properties, 1);
    auto* buttons = new QHBoxLayout;
    auto* addAttribute = new QPushButton(tr("+ Attribute"), m_form);
    connect(addAttribute, &QPushButton::clicked, this, [this]() { addProperty(false); });
    buttons->addWidget(addAttribute);
    auto* addRelationship = new QPushButton(tr("+ Relationship"), m_form);
    connect(addRelationship, &QPushButton::clicked, this, [this]() { addProperty(true); });
    buttons->addWidget(addRelationship);
    auto* remove = new QPushButton(tr("Remove"), m_form);
    connect(remove, &QPushButton::clicked, this, [this]() { removeProperty(m_properties->currentRow()); });
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);
}

QString SchemaLibraryEditor::library() const { return m_library; }

bool SchemaLibraryEditor::open(const QString& library, QString* error) {
    const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(library.toStdString());
    if (!location) {
        if (error) *error = tr("No schema library %1").arg(library);
        return false;
    }
    SchemaLibraryDef definition;
    std::string why;
    if (!readSchemaLibrary(location->source(), &definition, &why)) {
        if (error) *error = qs(why);
        return false;
    }
    m_library = library;
    m_known = stampOf(qs(location->source()));
    m_def = std::move(definition);
    m_modified = false;
    m_current = m_def.classes.empty() ? -1 : 0;
    m_title->setText(tr("Library %1").arg(library));
    const bool writable = location->writable;
    m_save->setEnabled(writable);
    m_generate->setEnabled(writable);
    refreshClassList();
    showClass();
    refreshIssues();
    refreshStatus();
    Q_EMIT libraryChanged();  // the watcher follows the file
    return true;
}

bool SchemaLibraryEditor::reload(QString* error) {
    const QString current = currentClass();
    if (!open(m_library, error)) return false;
    if (!current.isEmpty()) selectClass(current);
    return true;
}

bool SchemaLibraryEditor::save(QString* error) {
    const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(m_library.toStdString());
    if (!location) {
        if (error) *error = tr("No schema library %1").arg(m_library);
        return false;
    }
    // Someone else wrote it since (Edit as Text, Promote to Schema): their version
    // first, unless the form has edits of its own, and then only if told to.
    const QString path = qs(location->source());
    if (!m_known.isEmpty() && stampOf(path) != m_known) {
        if (!m_modified) return reload(error);
        if (QMessageBox::question(this, tr("Save"),
                                  tr("schema.usda changed on disk since the form read it. Overwrite it with the form's?"),
                                  QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save) {
            if (error) *error = tr("Not saved: schema.usda changed on disk. More > Reload shows it");
            return false;
        }
    }
    std::string why;
    if (!writeSchemaLibrary(m_def, location->source(), &why)) {
        if (error) *error = qs(why);
        return false;
    }
    m_known = stampOf(path);
    m_modified = false;
    refreshStatus();
    Q_EMIT libraryChanged();
    return true;
}

bool SchemaLibraryEditor::maybeSave() {
    if (!m_modified) return true;
    const auto answer = QMessageBox::question(this, tr("Schema Library %1").arg(m_library), tr("Save the edits to %1?").arg(m_library),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Discard) {
        m_modified = false;
        return true;
    }
    QString error;
    if (!save(&error)) {
        QMessageBox::warning(this, tr("Save"), error);
        return false;
    }
    return true;
}

SchemaClassDef* SchemaLibraryEditor::current() {
    return m_current >= 0 && m_current < int(m_def.classes.size()) ? &m_def.classes[std::size_t(m_current)] : nullptr;
}

QString SchemaLibraryEditor::currentClass() const {
    return m_current >= 0 && m_current < int(m_def.classes.size()) ? qs(m_def.classes[std::size_t(m_current)].name) : QString();
}

bool SchemaLibraryEditor::selectClass(const QString& name) {
    for (int i = 0; i < int(m_def.classes.size()); ++i) {
        if (qs(m_def.classes[std::size_t(i)].name) != name) continue;
        m_classes->setCurrentRow(i);
        m_current = i;
        showClass();
        return true;
    }
    return false;
}

void SchemaLibraryEditor::refreshClassList() {
    m_updating = true;
    m_classes->clear();
    for (const SchemaClassDef& c : m_def.classes) {
        auto* item = new QListWidgetItem(qs(c.name), m_classes);
        item->setToolTip(kindLabel(c.kind));
    }
    m_classes->setCurrentRow(m_current);
    m_updating = false;
}

void SchemaLibraryEditor::showClass() {
    const SchemaClassDef* c = current();
    m_form->setEnabled(c != nullptr);
    m_removeClass->setEnabled(c != nullptr);
    m_updating = true;
    // The bases to pick from: USD's prim types and this library's.
    m_base->clear();
    QStringList bases{QStringLiteral("Typed")};
    for (const SchemaInfo& info : allSchemas()) {
        if (isTyped(info.kind)) bases << qs(info.name);
    }
    for (const SchemaClassDef& other : m_def.classes) {
        if (!isApiKind(other.kind) && (!c || other.name != c->name)) bases << qs(other.name);
    }
    bases.removeDuplicates();
    bases.sort();
    m_base->addItems(bases);
    QStringList apis;
    for (const SchemaInfo& info : allSchemas()) {
        if (info.kind == SchemaInfo::Kind::SingleApplyAPI || info.kind == SchemaInfo::Kind::MultipleApplyAPI) apis << qs(info.name);
    }
    for (const SchemaClassDef& other : m_def.classes) {
        if (isApiKind(other.kind)) apis << qs(other.name);
    }
    m_builtIn->setCompleter(new QCompleter(apis, m_builtIn));
    if (c) {
        m_name->setText(qs(c->name));
        m_kind->setCurrentIndex(m_kind->findData(int(c->kind)));
        m_base->setEditText(qs(c->base.empty() ? std::string("Typed") : c->base));
        m_documentation->setPlainText(qs(c->documentation));
        m_builtIn->setText(joined(c->builtInAPIs));
        m_autoApply->setText(joined(c->autoApplyTo));
        m_canApply->setText(joined(c->canOnlyApplyTo));
        m_prefix->setText(qs(c->propertyNamespacePrefix));
    } else {
        for (QLineEdit* edit : {m_name, m_builtIn, m_autoApply, m_canApply, m_prefix}) edit->clear();
        m_documentation->clear();
    }
    const bool api = c && isApiKind(c->kind);
    m_baseLabel->setVisible(!api);
    m_base->setVisible(!api);
    for (QWidget* widget : std::initializer_list<QWidget*>{m_autoApplyLabel, m_autoApply, m_canApplyLabel, m_canApply}) widget->setVisible(api);
    const bool multiple = c && c->kind == SchemaInfo::Kind::MultipleApplyAPI;
    m_prefixLabel->setVisible(multiple);
    m_prefix->setVisible(multiple);
    m_updating = false;
    showProperties();
}

void SchemaLibraryEditor::showProperties() {
    m_updating = true;
    m_properties->setRowCount(0);
    if (const SchemaClassDef* c = current()) {
        const QStringList types = valueTypes();
        for (const SchemaPropertyDef& p : c->properties) {
            const int row = m_properties->rowCount();
            m_properties->insertRow(row);
            m_properties->setItem(row, Name, new QTableWidgetItem(qs(p.name)));
            auto* type = new QComboBox(m_properties);
            type->addItem(QLatin1String(kRelationship));
            type->addItems(types);
            type->setCurrentText(p.relationship ? QLatin1String(kRelationship) : qs(p.typeName));
            connect(type, &QComboBox::currentTextChanged, this, [this, type]() {
                // The row it's in now: rows move as others are removed.
                for (int r = 0; r < m_properties->rowCount(); ++r) {
                    if (m_properties->cellWidget(r, Type) == type && !m_updating) propertyEdited(r);
                }
            });
            m_properties->setCellWidget(row, Type, type);
            auto* uniform = new QTableWidgetItem;
            uniform->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            uniform->setCheckState(p.uniform ? Qt::Checked : Qt::Unchecked);
            m_properties->setItem(row, Uniform, uniform);
            m_properties->setItem(row, Fallback, new QTableWidgetItem(qs(p.fallback)));
            m_properties->setItem(row, Tokens, new QTableWidgetItem(joined(p.allowedTokens)));
            m_properties->setItem(row, DisplayName, new QTableWidgetItem(qs(p.displayName)));
            m_properties->setItem(row, DisplayGroup, new QTableWidgetItem(qs(p.displayGroup)));
            m_properties->setItem(row, Documentation, new QTableWidgetItem(qs(p.documentation)));
        }
    }
    m_properties->resizeColumnsToContents();
    m_updating = false;
}

void SchemaLibraryEditor::classEdited() {
    if (m_updating) return;
    SchemaClassDef* c = current();
    if (!c) return;
    const std::string name = m_name->text().trimmed().toStdString();
    const auto kind = SchemaInfo::Kind(m_kind->currentData().toInt());
    const bool kindChanged = kind != c->kind;
    c->name = name;
    c->kind = kind;
    c->base = isApiKind(kind) ? std::string("APISchemaBase") : m_base->currentText().trimmed().toStdString();
    c->documentation = m_documentation->toPlainText().trimmed().toStdString();
    c->builtInAPIs = split(m_builtIn->text());
    c->autoApplyTo = isApiKind(kind) ? split(m_autoApply->text()) : std::vector<std::string>();
    c->canOnlyApplyTo = isApiKind(kind) ? split(m_canApply->text()) : std::vector<std::string>();
    c->propertyNamespacePrefix =
        kind == SchemaInfo::Kind::MultipleApplyAPI ? m_prefix->text().trimmed().toStdString() : std::string();
    if (QListWidgetItem* item = m_classes->item(m_current)) {
        item->setText(qs(name));
        item->setToolTip(kindLabel(kind));
    }
    if (kindChanged) showClass();  // which fields there are
    markModified();
}

void SchemaLibraryEditor::propertyEdited(int row) {
    SchemaClassDef* c = current();
    if (!c || row < 0 || row >= int(c->properties.size())) return;
    SchemaPropertyDef& p = c->properties[std::size_t(row)];
    auto text = [this, row](int column) {
        const QTableWidgetItem* item = m_properties->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    p.name = text(Name).toStdString();
    const auto* type = qobject_cast<QComboBox*>(m_properties->cellWidget(row, Type));
    p.relationship = type && type->currentText() == QLatin1String(kRelationship);
    p.typeName = p.relationship ? std::string() : (type ? type->currentText().toStdString() : p.typeName);
    if (const QTableWidgetItem* uniform = m_properties->item(row, Uniform)) p.uniform = uniform->checkState() == Qt::Checked;
    p.fallback = p.relationship ? std::string() : text(Fallback).toStdString();
    p.allowedTokens = split(text(Tokens));
    p.displayName = text(DisplayName).toStdString();
    p.displayGroup = text(DisplayGroup).toStdString();
    p.documentation = text(Documentation).toStdString();
    markModified();
}

void SchemaLibraryEditor::markModified() {
    m_modified = true;
    refreshStatus();
    m_checkTimer->start();
}

void SchemaLibraryEditor::addClass(SchemaInfo::Kind kind) {
    const std::string prefix = libraryTypePrefix(m_def);
    const std::string stem = prefix + (kind == SchemaInfo::Kind::AbstractTyped ? "Base" : "Thing") + (isApiKind(kind) ? "API" : "");
    std::string name = stem;
    auto taken = [this](const std::string& n) {
        return std::any_of(m_def.classes.begin(), m_def.classes.end(), [&](const SchemaClassDef& c) { return c.name == n; }) ||
               !schemaInfo(n).name.empty();
    };
    for (int n = 2; taken(name); ++n) {
        name = isApiKind(kind) ? stem.substr(0, stem.size() - 3) + std::to_string(n) + "API" : stem + std::to_string(n);
    }
    SchemaClassDef c;
    c.name = name;
    c.kind = kind;
    c.base = isApiKind(kind) ? "APISchemaBase" : (schemaInfo("Xformable").name.empty() ? "Typed" : "Xformable");
    if (kind == SchemaInfo::Kind::MultipleApplyAPI) {
        c.propertyNamespacePrefix = m_def.name;
        if (!c.propertyNamespacePrefix.empty()) c.propertyNamespacePrefix[0] = char(std::tolower(c.propertyNamespacePrefix[0]));
    }
    m_def.classes.push_back(c);
    m_current = int(m_def.classes.size()) - 1;
    refreshClassList();
    showClass();
    markModified();
}

void SchemaLibraryEditor::removeCurrentClass() {
    if (!current()) return;
    m_def.classes.erase(m_def.classes.begin() + m_current);
    m_current = std::min(m_current, int(m_def.classes.size()) - 1);
    refreshClassList();
    showClass();
    markModified();
}

int SchemaLibraryEditor::addProperty(bool relationship) {
    SchemaClassDef* c = current();
    if (!c) return -1;
    // In the library's namespace; a multiple-apply API's go under its prefix anyway.
    const std::string stem = c->kind == SchemaInfo::Kind::MultipleApplyAPI ? std::string("property") : m_def.name + ":property";
    std::string name = stem;
    auto taken = [c](const std::string& n) {
        return std::any_of(c->properties.begin(), c->properties.end(), [&](const SchemaPropertyDef& p) { return p.name == n; });
    };
    for (int n = 2; taken(name); ++n) name = stem + std::to_string(n);
    SchemaPropertyDef p;
    p.name = name;
    p.relationship = relationship;
    p.typeName = relationship ? std::string() : std::string("float");
    c->properties.push_back(p);
    showProperties();
    markModified();
    return m_properties->rowCount() - 1;
}

void SchemaLibraryEditor::removeProperty(int row) {
    SchemaClassDef* c = current();
    if (!c || row < 0 || row >= int(c->properties.size())) return;
    c->properties.erase(c->properties.begin() + row);
    showProperties();
    markModified();
}

std::vector<SchemaIssue> SchemaLibraryEditor::issues() const {
    std::vector<SchemaLibraryDef> bases;
    for (const std::string& folder : SchemaLibraries::instance().baseFolders(m_def)) {
        SchemaLibraryDef base;
        if (findSchemaLibrary(folder, &base, nullptr)) bases.push_back(std::move(base));
    }
    return checkSchemaLibrary(m_def, bases);
}

void SchemaLibraryEditor::refreshIssues() {
    m_checkTimer->stop();
    m_issues->clear();
    for (const SchemaIssue& issue : issues()) {
        auto* item = new QListWidgetItem(qs(issueText(issue)), m_issues);
        item->setForeground(issue.severity == SchemaIssue::Severity::Error ? errorColor() : warningColor());
        item->setData(Qt::UserRole, qs(issue.schema));
    }
    m_issues->setVisible(m_issues->count() > 0);
}

void SchemaLibraryEditor::refreshStatus() {
    const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(m_library.toStdString());
    QString text;
    if (m_generating) {
        text = tr("Generating...");
    } else if (!location) {
        text = tr("Not found");
    } else if (!location->writable) {
        text = tr("Read-only: its folder can't be written");
    } else if (!location->generated) {
        text = tr("Not generated yet");
    } else if (location->changed) {
        text = tr("Changed since it was generated");
    } else if (location->waitingForRestart) {
        text = tr("Generated: it loads at the next start");
    } else {
        text = tr("Loaded");
    }
    if (m_modified && !m_generating) text += tr(" · not saved");
    m_status->setText(text);
    m_restart->setVisible(location && location->waitingForRestart && !location->changed && hooks().restart);
}

void SchemaLibraryEditor::generate(bool ask) {
    if (m_generating) return;
    const std::vector<SchemaIssue> found = issues();
    refreshIssues();
    if (hasErrors(found)) {
        m_status->setText(tr("Fix the errors before generating"));
        m_lastResult = {};
        m_lastResult.error = "It has errors";
        Q_EMIT generated(false);
        return;
    }
    if (ask) {
        const std::vector<std::string> changes = schemaLibraryChanges(m_def);
        if (!changes.empty()) {
            QStringList lines;
            for (std::size_t i = 0; i < changes.size() && i < 12; ++i) lines << QStringLiteral("    ") + qs(changes[i]);
            if (changes.size() > 12) lines << tr("    and %1 more").arg(changes.size() - 12);
            QMessageBox box(QMessageBox::Question, tr("Generate %1").arg(m_library),
                            tr("Generating changes what files that use %1 read.").arg(m_library), QMessageBox::Cancel, this);
            box.setInformativeText(lines.join(QLatin1Char('\n')) +
                                   tr("\n\nA new version of a schema (Name_1) leaves files using the old one as they are."));
            QPushButton* go = box.addButton(tr("Generate"), QMessageBox::AcceptRole);
            box.exec();
            if (box.clickedButton() != go) return;
        }
    }
    QString error;
    if (!save(&error)) {
        QMessageBox::warning(this, tr("Generate"), error);
        return;
    }
    const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(m_library.toStdString());
    if (!location) return;
    const SchemaPython python = hooks().python;
    const std::string usdFolder = location->usdFolder;
    const std::string library = m_library.toStdString();
    const std::vector<std::string> bases = SchemaLibraries::instance().baseFolders(m_def);
    m_generating = true;
    m_generate->setEnabled(false);
    m_save->setEnabled(false);
    refreshStatus();
    auto result = std::make_shared<SchemaGenerateResult>();
    QThread* thread = QThread::create([result, usdFolder, library, python, bases]() {
        *result = generateSchemaLibrary(usdFolder, library, python, bases);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, result]() { finishGenerate(*result); });
    // The app mustn't exit under a running thread.
    connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread]() { thread->wait(); });
    thread->start();
}

void SchemaLibraryEditor::finishGenerate(const SchemaGenerateResult& result) {
    m_generating = false;
    m_lastResult = result;
    m_generate->setEnabled(true);
    m_save->setEnabled(true);
    refreshIssues();
    if (!result.ok) {
        auto* item = new QListWidgetItem(tr("Generate: %1").arg(qs(result.error)));
        item->setForeground(errorColor());
        m_issues->insertItem(0, item);
        if (!result.output.empty() && result.output != result.error) {
            m_issues->insertItem(1, new QListWidgetItem(tr("usdGenSchema said: %1").arg(qs(result.output))));
        }
        m_issues->setVisible(true);
    }
    refreshStatus();
    if (result.ok) m_status->setText(tr("Generated: it loads at the next start"));
    Q_EMIT libraryChanged();
    Q_EMIT generated(result.ok);
}

}  // namespace usdcc::usd
