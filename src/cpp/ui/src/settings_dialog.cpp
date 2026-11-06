#include "usdcc/ui/settings_dialog.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scaled_metrics.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <functional>
#include <memory>
#include <utility>

namespace usdcc::ui {

namespace {

using core::SettingDefinition;
using core::Settings;
using core::SettingType;

QString qs(const std::string& text) { return QString::fromStdString(text); }

// Categories the app knows come first, in this order; any other (a plugin's)
// follows in the order it was defined.
const QStringList& knownCategoryOrder() {
    static const QStringList order = {QStringLiteral("Appearance"), QStringLiteral("Viewport"),
                                      QStringLiteral("Tools"), QStringLiteral("Shortcuts"), QStringLiteral("Notifications"),
                                      QStringLiteral("Python")};
    return order;
}

QString valueText(const core::SettingValue& value) {
    if (const bool* b = std::get_if<bool>(&value)) return *b ? QStringLiteral("on") : QStringLiteral("off");
    if (const int* i = std::get_if<int>(&value)) return QString::number(*i);
    if (const double* d = std::get_if<double>(&value)) return QString::number(*d);
    const QString text = qs(std::get<std::string>(value));
    return text.isEmpty() ? QObject::tr("none") : text;
}

// The other Shortcut settings that use `key`'s sequence.
QStringList shortcutConflicts(const Settings& settings, const SettingDefinition& definition) {
    const QKeySequence sequence = settingShortcut(definition.key, settings);
    QStringList labels;
    if (sequence.isEmpty()) return labels;
    for (const SettingDefinition* other : settings.definitions()) {
        if (other->key != definition.key && other->type == SettingType::Shortcut &&
            settingShortcut(other->key, settings) == sequence) {
            labels << qs(other->label.empty() ? other->key : other->label);
        }
    }
    return labels;
}

QIcon swatch(const QColor& color) {
    QPixmap pixmap(scaled::size(14, 14));
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(255, 255, 255, 60));
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(pixmap.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    return QIcon(pixmap);
}

// An editor for one setting: the widget, and how to show the setting's value
// in it (called on every change of the setting, wherever it came from).
struct Editor {
    QWidget* widget = nullptr;
    std::function<void()> refresh;
};

Editor makeEditor(Settings& settings, const SettingDefinition& definition, QWidget* parent) {
    const std::string key = definition.key;
    switch (definition.type) {
    case SettingType::Bool: {
        auto* box = new QCheckBox(parent);
        QObject::connect(box, &QCheckBox::toggled, box, [&settings, key](bool on) { settings.setValue(key, on); });
        return {box, [&settings, key, box]() {
                    const QSignalBlocker block(box);
                    box->setChecked(settings.get<bool>(key));
                }};
    }
    case SettingType::Int: {
        auto* spin = new QSpinBox(parent);
        const bool ranged = definition.maximum > definition.minimum;
        spin->setRange(ranged ? int(definition.minimum) : INT_MIN, ranged ? int(definition.maximum) : INT_MAX);
        spin->setKeyboardTracking(false);
        scaled::minimumWidth(spin, 90);
        QObject::connect(spin, &QSpinBox::valueChanged, spin, [&settings, key](int value) { settings.setValue(key, value); });
        return {spin, [&settings, key, spin]() {
                    const QSignalBlocker block(spin);
                    spin->setValue(settings.get<int>(key));
                }};
    }
    case SettingType::Float: {
        auto* spin = new QDoubleSpinBox(parent);
        const bool ranged = definition.maximum > definition.minimum;
        spin->setRange(ranged ? definition.minimum : -1e12, ranged ? definition.maximum : 1e12);
        spin->setDecimals(3);
        spin->setKeyboardTracking(false);
        scaled::minimumWidth(spin, 90);
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin,
                         [&settings, key](double value) { settings.setValue(key, value); });
        return {spin, [&settings, key, spin]() {
                    const QSignalBlocker block(spin);
                    spin->setValue(settings.get<double>(key));
                }};
    }
    case SettingType::Choice: {
        auto* combo = new QComboBox(parent);
        for (const std::string& choice : definition.choices) combo->addItem(qs(choice));
        scaled::minimumWidth(combo, 140);
        QObject::connect(combo, &QComboBox::activated, combo, [&settings, key, combo](int index) {
            settings.setValue(key, combo->itemText(index).toStdString());
        });
        return {combo, [&settings, key, combo]() {
                    const QSignalBlocker block(combo);
                    combo->setCurrentText(qs(settings.get<std::string>(key)));
                }};
    }
    case SettingType::String: {
        auto* edit = new QLineEdit(parent);
        scaled::minimumWidth(edit, 200);
        QObject::connect(edit, &QLineEdit::editingFinished, edit,
                         [&settings, key, edit]() { settings.setValue(key, edit->text().toStdString()); });
        return {edit, [&settings, key, edit]() {
                    const QSignalBlocker block(edit);
                    edit->setText(qs(settings.get<std::string>(key)));
                }};
    }
    case SettingType::Folder: {
        auto* box = new QWidget(parent);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        scaled::spacing(row, 4);
        auto* edit = new QLineEdit(box);
        scaled::minimumWidth(edit, 200);
        row->addWidget(edit, 1);
        auto* browse = new QToolButton(box);
        browse->setObjectName(QStringLiteral("settingBrowse"));
        browse->setText(QObject::tr("Browse..."));
        row->addWidget(browse);
        QObject::connect(edit, &QLineEdit::editingFinished, edit,
                         [&settings, key, edit]() { settings.setValue(key, edit->text().toStdString()); });
        QObject::connect(browse, &QToolButton::clicked, browse, [&settings, key, edit, box]() {
            const QString folder = QFileDialog::getExistingDirectory(box->window(), QObject::tr("Choose a Folder"), edit->text());
            if (!folder.isEmpty()) settings.setValue(key, QDir::toNativeSeparators(folder).toStdString());
        });
        return {box, [&settings, key, edit]() {
                    const QSignalBlocker block(edit);
                    edit->setText(qs(settings.get<std::string>(key)));
                }};
    }
    case SettingType::FolderList: {
        // The value's folders, between the fixed ones that come before and after
        // them; buttons to add, remove and reorder the value's.
        auto* box = new QWidget(parent);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        scaled::spacing(row, 4);
        auto* list = new QListWidget(box);
        list->setObjectName(QStringLiteral("settingFolderList"));
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        scaled::minimumWidth(list, 200);
        scaled::fixedHeight(list, 120);
        row->addWidget(list, 1);
        auto* buttons = new QVBoxLayout;
        buttons->setContentsMargins(0, 0, 0, 0);
        scaled::spacing(buttons, 4);
        const auto button = [box, buttons](const char* name, const QString& text, const QString& tip) {
            auto* made = new QToolButton(box);
            made->setObjectName(QString::fromLatin1(name));
            made->setText(text);
            made->setToolTip(tip);
            made->setToolButtonStyle(Qt::ToolButtonTextOnly);
            made->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            buttons->addWidget(made);
            return made;
        };
        QToolButton* add = button("settingFolderAdd", QObject::tr("Add..."), QObject::tr("Add a folder at the end"));
        QToolButton* remove = button("settingFolderRemove", QObject::tr("Remove"), QObject::tr("Remove the selected folder"));
        QToolButton* up = button("settingFolderUp", QObject::tr("Move Up"), QObject::tr("Look in the selected folder sooner"));
        QToolButton* down = button("settingFolderDown", QObject::tr("Move Down"), QObject::tr("Look in the selected folder later"));
        buttons->addStretch(1);
        row->addLayout(buttons);

        // The value's folders, and the one to select after the next refresh.
        const auto folders = [&settings, key]() {
            QStringList result;
            for (const QString& folder : qs(settings.get<std::string>(key)).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                if (!folder.trimmed().isEmpty()) result << folder.trimmed();
            }
            return result;
        };
        const auto store = [&settings, key](const QStringList& value) {
            settings.setValue(key, value.join(QLatin1Char(';')).toStdString());
        };
        auto select = std::make_shared<int>(-1);
        const auto selected = [list]() {
            const QListWidgetItem* item = list->currentItem();
            return item && item->data(Qt::UserRole).isValid() ? item->data(Qt::UserRole).toInt() : -1;
        };
        const auto updateButtons = [folders, selected, remove, up, down]() {
            const int index = selected();
            remove->setEnabled(index >= 0);
            up->setEnabled(index > 0);
            down->setEnabled(index >= 0 && index + 1 < folders().size());
        };
        QObject::connect(list, &QListWidget::currentRowChanged, list, [updateButtons]() { updateButtons(); });
        QObject::connect(add, &QToolButton::clicked, add, [folders, store, select, box]() {
            const QString chosen = QFileDialog::getExistingDirectory(box->window(), QObject::tr("Choose a Folder"));
            if (chosen.isEmpty()) return;
            QStringList value = folders();
            const QString folder = QDir::toNativeSeparators(chosen);
            if (!value.contains(folder, Qt::CaseInsensitive)) value << folder;
            *select = int(value.indexOf(folder));
            store(value);
        });
        QObject::connect(remove, &QToolButton::clicked, remove, [folders, store, selected, select]() {
            QStringList value = folders();
            const int index = selected();
            if (index < 0 || index >= value.size()) return;
            value.removeAt(index);
            *select = std::min(index, int(value.size()) - 1);
            store(value);
        });
        const auto move = [folders, store, selected, select](int by) {
            QStringList value = folders();
            const int index = selected();
            const int to = index + by;
            if (index < 0 || to < 0 || to >= value.size()) return;
            value.move(index, to);
            *select = to;
            store(value);
        };
        QObject::connect(up, &QToolButton::clicked, up, [move]() { move(-1); });
        QObject::connect(down, &QToolButton::clicked, down, [move]() { move(1); });

        const std::vector<core::SettingDefinition::FixedFolder> fixed = definition.fixedFolders;
        return {box, [folders, fixed, list, select, updateButtons]() {
                    const QSignalBlocker block(list);
                    list->clear();
                    const auto missing = [](const QString& folder) {
                        return QFileInfo(folder).isDir() ? QString() : QObject::tr(" (missing)");
                    };
                    const auto addFixed = [&](bool after) {
                        for (const auto& folder : fixed) {
                            if (folder.after != after) continue;
                            const QString path = QDir::toNativeSeparators(qs(folder.folder));
                            auto* item = new QListWidgetItem(QStringLiteral("%1: %2%3").arg(qs(folder.label), path, missing(path)), list);
                            item->setFlags(Qt::NoItemFlags);  // greyed: always there
                            item->setToolTip(QObject::tr("Always used; can't be removed"));
                        }
                    };
                    addFixed(false);
                    const QStringList value = folders();
                    for (int i = 0; i < value.size(); ++i) {
                        auto* item = new QListWidgetItem(value[i] + missing(value[i]), list);
                        item->setData(Qt::UserRole, i);
                        item->setToolTip(value[i]);
                        if (i == *select) list->setCurrentItem(item);
                    }
                    addFixed(true);
                    *select = -1;
                    updateButtons();
                }};
    }
    case SettingType::Color: {
        auto* button = new QToolButton(parent);
        button->setObjectName(QStringLiteral("settingColorButton"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setToolTip(QObject::tr("Pick a color"));
        scaled::minimumWidth(button, 110);
        QObject::connect(button, &QToolButton::clicked, button, [&settings, key, button]() {
            const QColor original = settingColor(key, settings);
            QColorDialog dialog(original, button->window());
            // Qt's own dialog: the native one doesn't report the colour while
            // it is being picked, so nothing could follow it live.
            dialog.setOption(QColorDialog::DontUseNativeDialog);
            dialog.setWindowTitle(QObject::tr("Choose a Color"));
            // Restyling the whole app on every mouse move would lag: apply
            // the latest colour at most every 100 ms.
            QTimer apply;
            apply.setSingleShot(true);
            apply.setInterval(100);
            QColor pending = original;
            QObject::connect(&apply, &QTimer::timeout, &dialog, [&settings, key, &pending]() {
                settings.setValue(key, pending.name().toStdString());
            });
            QObject::connect(&dialog, &QColorDialog::currentColorChanged, &dialog, [&pending, &apply](const QColor& color) {
                pending = color;
                if (!apply.isActive()) apply.start();
            });
            const bool accepted = dialog.exec() == QDialog::Accepted;
            apply.stop();
            settings.setValue(key, (accepted ? dialog.selectedColor() : original).name().toStdString());
        });
        return {button, [&settings, key, button]() {
                    const QColor color = settingColor(key, settings);
                    button->setIcon(swatch(color));
                    button->setText(color.name());
                }};
    }
    case SettingType::Shortcut: {
        auto* edit = new QKeySequenceEdit(parent);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        scaled::minimumWidth(edit, 140);
        QObject::connect(edit, &QKeySequenceEdit::keySequenceChanged, edit,
                         [&settings, key](const QKeySequence& sequence) {
                             settings.setValue(key, shortcutText(sequence));
                         });
        return {edit, [&settings, key, edit]() {
                    const QSignalBlocker block(edit);
                    edit->setKeySequence(settingShortcut(key, settings));
                }};
    }
    }
    return {new QWidget(parent), []() {}};
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent, core::Settings& settings) : QDialog(parent), m_settings(settings) {
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    resize(scaled::size(760, 520));

    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("settingsCategories"));
    scaled::fixedWidth(m_categories, 170);
    m_pages = new QStackedWidget(this);
    connect(m_categories, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    auto* body = new QHBoxLayout;
    scaled::spacing(body, 12);
    body->addWidget(m_categories);
    body->addWidget(m_pages, 1);

    m_resetPage = new QPushButton(tr("Reset Page to Defaults"), this);
    // Not the default button: Enter must never reset a page.
    m_resetPage->setAutoDefault(false);
    m_resetPage->setToolTip(tr("Every setting on this page back to its default"));
    connect(m_resetPage, &QPushButton::clicked, this, [this]() {
        const QString category = currentCategory();
        if (!category.isEmpty()) m_settings.resetCategory(category.toStdString());
    });
    auto* note = new QLabel(tr("Changes apply immediately."), this);
    note->setObjectName(QStringLiteral("propertyNote"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    // No default button: Enter in a field (the UI scale's spin box) must not
    // close the dialog — see keyPressEvent().
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(m_resetPage);
    bottom->addWidget(note, 1);
    bottom->addWidget(buttons);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addLayout(bottom);

    // A plugin may define settings while the dialog is open: rebuild the
    // pages, once for a whole batch of definitions.
    m_definedConnection = m_settings.defined.connect([this](const std::string&) { scheduleRebuild(); });
    rebuild();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::keyPressEvent(QKeyEvent* event) {
    // QDialog would click the default button. The editor has already
    // committed its value by now (spin boxes and line edits do on Enter), and
    // that is all Enter should do here.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

SettingsDialog* SettingsDialog::showDialog(QWidget* parent) {
    static QPointer<SettingsDialog> dialog;
    if (!dialog) {
        dialog = new SettingsDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return dialog;
}

QStringList SettingsDialog::categories() const {
    QStringList result;
    for (int row = 0; row < m_categories->count(); ++row) result << m_categories->item(row)->text();
    return result;
}

QString SettingsDialog::currentCategory() const {
    const QListWidgetItem* item = m_categories->currentItem();
    return item ? item->text() : QString();
}

bool SettingsDialog::showCategory(const QString& category) {
    const QList<QListWidgetItem*> items = m_categories->findItems(category, Qt::MatchExactly);
    if (items.isEmpty()) return false;
    m_categories->setCurrentItem(items.front());
    return true;
}

void SettingsDialog::scheduleRebuild() {
    if (m_rebuildScheduled) return;
    m_rebuildScheduled = true;
    // Later, not inside Settings::defined: the rebuild deletes editors whose
    // own connections may be part of the notification in progress.
    QTimer::singleShot(0, this, [this]() {
        m_rebuildScheduled = false;
        rebuild();
    });
}

void SettingsDialog::rebuild() {
    const QString current = currentCategory();

    QStringList categories;
    for (const QString& known : knownCategoryOrder()) {
        if (!m_settings.definitions(known.toStdString()).empty()) categories << known;
    }
    for (const std::string& category : m_settings.categories()) {
        if (!categories.contains(qs(category))) categories << qs(category);
    }

    const QSignalBlocker block(m_categories);
    m_categories->clear();
    while (m_pages->count() > 0) {
        QWidget* page = m_pages->widget(0);
        m_pages->removeWidget(page);
        delete page;
    }
    for (const QString& category : categories) {
        m_categories->addItem(category);
        m_pages->addWidget(buildPage(category.toStdString()));
    }
    const int row = std::max(0, int(categories.indexOf(current)));
    if (row < m_categories->count()) {
        m_categories->setCurrentRow(row);
        m_pages->setCurrentIndex(row);
    }
    m_resetPage->setEnabled(m_categories->count() > 0);
}

QWidget* SettingsDialog::buildPage(const std::string& category) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("settingsPage"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsPageContent"));
    auto* layout = new QVBoxLayout(content);
    scaled::margins(layout, 4, 0, 8, 0);
    scaled::spacing(layout, 6);

    auto* title = new QLabel(qs(category), content);
    title->setObjectName(QStringLiteral("settingsPageTitle"));
    layout->addWidget(title);

    // Groups in the order their first setting was defined.
    std::vector<std::string> groups;
    const std::vector<const SettingDefinition*> definitions = m_settings.definitions(category);
    for (const SettingDefinition* definition : definitions) {
        if (std::find(groups.begin(), groups.end(), definition->group) == groups.end()) {
            groups.push_back(definition->group);
        }
    }

    for (const std::string& group : groups) {
        if (!group.empty()) {
            auto* heading = new QLabel(qs(group), content);
            heading->setObjectName(QStringLiteral("settingsGroupTitle"));
            layout->addWidget(heading);
        }
        auto* grid = new QGridLayout;
        scaled::spacing(grid, 8);
        grid->setColumnStretch(1, 1);
        int row = 0;
        for (const SettingDefinition* definition : definitions) {
            if (definition->group != group) continue;
            const std::string key = definition->key;

            auto* label = new QLabel(qs(definition->label.empty() ? key : definition->label), content);
            label->setObjectName(QStringLiteral("settingLabel"));
            label->setToolTip(qs(key));
            scaled::minimumWidth(label, 150);

            Editor editor = makeEditor(m_settings, *definition, content);
            editor.widget->setObjectName(editor.widget->objectName().isEmpty()
                                             ? QStringLiteral("settingEditor")
                                             : editor.widget->objectName());
            editor.widget->setProperty("settingKey", qs(key));
            auto* editorRow = new QHBoxLayout;
            editorRow->setContentsMargins(0, 0, 0, 0);
            // Text and paths get the room there is; the others keep their size.
            if (definition->type == SettingType::String || definition->type == SettingType::Folder ||
                definition->type == SettingType::FolderList) {
                editorRow->addWidget(editor.widget, 1);
            } else {
                editorRow->addWidget(editor.widget);
                editorRow->addStretch(1);
            }

            auto* reset = new QToolButton(content);
            reset->setObjectName(QStringLiteral("settingReset"));
            reset->setText(tr("Reset"));
            reset->setProperty("settingKey", qs(key));
            reset->setToolTip(tr("Back to the default: %1").arg(valueText(definition->defaultValue)));
            QSizePolicy keepSpace = reset->sizePolicy();
            keepSpace.setRetainSizeWhenHidden(true);
            reset->setSizePolicy(keepSpace);
            connect(reset, &QToolButton::clicked, this, [this, key]() { m_settings.reset(key); });

            grid->addWidget(label, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
            grid->addLayout(editorRow, row, 1);
            grid->addWidget(reset, row, 2);
            ++row;

            QLabel* conflict = nullptr;
            if (definition->type == SettingType::Shortcut) {
                conflict = new QLabel(content);
                conflict->setObjectName(QStringLiteral("settingConflict"));
                conflict->setProperty("settingKey", qs(key));
                grid->addWidget(conflict, row++, 1);
            }
            if (!definition->description.empty()) {
                auto* description = new QLabel(qs(definition->description), content);
                description->setObjectName(QStringLiteral("propertyNote"));
                description->setWordWrap(true);
                grid->addWidget(description, row++, 1, 1, 2);
            }
            QLabel* nextStart = nullptr;
            if (definition->appliesAtNextStart) {
                nextStart = new QLabel(tr("Takes effect at the next start."), content);
                nextStart->setObjectName(QStringLiteral("settingNextStart"));
                nextStart->setProperty("settingKey", qs(key));
                grid->addWidget(nextStart, row++, 1, 1, 2);
            }

            const auto refresh = [this, key, editor, reset, conflict, nextStart]() {
                editor.refresh();
                reset->setVisible(!m_settings.isDefault(key));
                if (nextStart) nextStart->setVisible(m_settings.isPendingNextStart(key));
                if (conflict) {
                    const SettingDefinition* current = m_settings.definition(key);
                    const QStringList others = current ? shortcutConflicts(m_settings, *current) : QStringList();
                    conflict->setText(others.isEmpty() ? QString() : tr("Also used by: %1").arg(others.join(", ")));
                    conflict->setVisible(!others.isEmpty());
                }
            };
            // Shortcut conflicts depend on the other shortcuts too.
            if (conflict) {
                onSettingsChanged(label, [refresh](const std::string&) { refresh(); }, m_settings);
            } else {
                onSettingChanged(label, key, refresh, m_settings);
            }
            refresh();
        }
        layout->addLayout(grid);
    }
    layout->addStretch(1);
    scroll->setWidget(content);
    return scroll;
}

}  // namespace usdcc::ui
