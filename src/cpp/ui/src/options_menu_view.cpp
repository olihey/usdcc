#include "usdcc/ui/options_menu_view.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>
#include <vector>

namespace usdcc::ui {

namespace {
QString plainText(const QAction* action) {
    QString text = action->text();
    text.remove(QLatin1Char('&'));
    return text;
}
}  // namespace

OptionsMenuView::OptionsMenuView(std::function<void(QMenu*)> populate, QWidget* parent)
    : QWidget(parent), m_populate(std::move(populate)) {
    setObjectName(QStringLiteral("optionsMenuView"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll);
    m_body = new QWidget(scroll);
    m_body->setObjectName(QStringLiteral("optionsMenuBody"));
    scroll->setWidget(m_body);
    new QVBoxLayout(m_body);
    rebuild();
}

OptionsMenuView::~OptionsMenuView() { delete m_menu; }

void OptionsMenuView::scheduleRebuild() {
    if (m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_scheduled = false;
        rebuild();
    });
}

void OptionsMenuView::rebuild() {
    ++m_rebuilds;
    // The old widgets and menu go once this turn is over: one of them may be what was clicked.
    auto* layout = static_cast<QVBoxLayout*>(m_body->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    if (m_menu) m_menu->deleteLater();
    // Not shown, and parented to nothing: as the options button's own menu, but kept.
    m_menu = new QMenu();
    if (m_populate) m_populate(m_menu);
    auto* content = new QWidget(m_body);
    auto* contentLayout = new QVBoxLayout(content);
    usdcc::ui::scaled::margins(contentLayout, 8, 6, 8, 6);
    usdcc::ui::scaled::spacing(contentLayout, 2);
    addEntries(m_menu, contentLayout, 0);
    // The drop-downs line up: their titles take the widest one's width.
    const QList<QLabel*> titles = content->findChildren<QLabel*>(QStringLiteral("optionsChoiceTitle"));
    int titleWidth = 0;
    for (QLabel* title : titles) titleWidth = qMax(titleWidth, title->sizeHint().width());
    for (QLabel* title : titles) title->setMinimumWidth(titleWidth);
    contentLayout->addStretch(1);
    layout->addWidget(content);
}

void OptionsMenuView::addEntries(QMenu* menu, QVBoxLayout* layout, int depth) {
    for (QAction* action : menu->actions()) {
        // What the panel changes on its own, such as a setting another panel shares.
        connect(action, &QAction::changed, this, &OptionsMenuView::scheduleRebuild, Qt::UniqueConnection);
        if (!action->isVisible()) continue;
        if (action->isSeparator()) {
            auto* line = new QFrame(layout->parentWidget());
            line->setObjectName(QStringLiteral("optionsSeparator"));
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Plain);
            layout->addWidget(line);
            continue;
        }
        if (QMenu* submenu = action->menu()) {
            if (addChoice(action, layout)) continue;
            // A group under its title, indented.
            auto* title = new QLabel(plainText(action), layout->parentWidget());
            title->setObjectName(QStringLiteral("optionsGroupTitle"));
            QFont font = title->font();
            font.setBold(true);
            title->setFont(font);
            layout->addWidget(title);
            auto* group = new QWidget(layout->parentWidget());
            auto* groupLayout = new QVBoxLayout(group);
            usdcc::ui::scaled::margins(groupLayout, 10, 0, 0, 4);
            usdcc::ui::scaled::spacing(groupLayout, 2);
            addEntries(submenu, groupLayout, depth + 1);
            layout->addWidget(group);
            continue;
        }
        QAbstractButton* button = nullptr;
        if (action->isCheckable()) {
            if (action->actionGroup() && action->actionGroup()->isExclusive()) {
                button = new QRadioButton(plainText(action), layout->parentWidget());
            } else {
                button = new QCheckBox(plainText(action), layout->parentWidget());
            }
            button->setChecked(action->isChecked());
        } else {
            auto* push = new QPushButton(plainText(action), layout->parentWidget());
            push->setFlat(true);
            push->setStyleSheet(QStringLiteral("text-align: left;"));
            button = push;
        }
        button->setObjectName(QStringLiteral("optionsEntry"));
        button->setIcon(action->icon());
        button->setToolTip(action->toolTip() != plainText(action) ? action->toolTip() : QString());
        button->setEnabled(action->isEnabled());
        // A radio button for what's already chosen does nothing; the others choose.
        const QPointer<QAction> target(action);
        connect(button, &QAbstractButton::clicked, this, [this, target]() {
            if (!target) return;
            target->trigger();
            scheduleRebuild();  // what it changed shows, as a menu's next opening would
        });
        layout->addWidget(button);
    }
}

bool OptionsMenuView::addChoice(QAction* submenuAction, QVBoxLayout* layout) {
    // One choice: every entry checkable and in the same exclusive group.
    QActionGroup* group = nullptr;
    std::vector<QPointer<QAction>> choices;
    for (QAction* action : submenuAction->menu()->actions()) {
        connect(action, &QAction::changed, this, &OptionsMenuView::scheduleRebuild, Qt::UniqueConnection);
        if (!action->isVisible()) continue;
        if (action->isSeparator() || action->menu() || !action->isCheckable() || !action->actionGroup() ||
            !action->actionGroup()->isExclusive() || (group && action->actionGroup() != group)) {
            return false;
        }
        group = action->actionGroup();
        choices.emplace_back(action);
    }
    if (choices.empty()) return false;

    auto* row = new QWidget(layout->parentWidget());
    row->setObjectName(QStringLiteral("optionsChoiceRow"));
    auto* rowLayout = new QHBoxLayout(row);
    usdcc::ui::scaled::margins(rowLayout, 0, 1, 0, 1);
    usdcc::ui::scaled::spacing(rowLayout, 6);
    auto* title = new QLabel(plainText(submenuAction), row);
    title->setObjectName(QStringLiteral("optionsChoiceTitle"));
    auto* combo = new QComboBox(row);
    combo->setObjectName(QStringLiteral("optionsChoice"));
    combo->setAccessibleName(plainText(submenuAction));
    // Long entries (camera paths) don't widen the column; the open list shows them whole.
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(8);
    combo->setFocusPolicy(Qt::StrongFocus);
    combo->installEventFilter(this);  // the wheel scrolls the column, not the choice
    title->setBuddy(combo);
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    int current = -1;
    for (int i = 0; i < int(choices.size()); ++i) {
        QAction* action = choices[size_t(i)];
        combo->addItem(action->icon(), plainText(action));
        if (action->toolTip() != plainText(action)) combo->setItemData(i, action->toolTip(), Qt::ToolTipRole);
        if (model && !action->isEnabled()) model->item(i)->setEnabled(false);
        if (action->isChecked()) current = i;
    }
    combo->setCurrentIndex(current);
    combo->setEnabled(submenuAction->isEnabled());
    if (submenuAction->toolTip() != plainText(submenuAction)) combo->setToolTip(submenuAction->toolTip());
    connect(combo, &QComboBox::activated, this, [this, choices](int index) {
        if (index < 0 || index >= int(choices.size()) || !choices[size_t(index)]) return;
        if (!choices[size_t(index)]->isChecked()) choices[size_t(index)]->trigger();
        scheduleRebuild();  // what it changed shows, as a menu's next opening would
    });
    rowLayout->addWidget(title);
    rowLayout->addWidget(combo, 1);
    layout->addWidget(row);
    return true;
}

bool OptionsMenuView::eventFilter(QObject* watched, QEvent* event) {
    // A drop-down that hasn't been clicked leaves the wheel to the column's scrolling.
    auto* combo = qobject_cast<QComboBox*>(watched);
    if (combo && event->type() == QEvent::Wheel && !combo->hasFocus()) {
        if (auto* scroll = findChild<QScrollArea*>()) QCoreApplication::sendEvent(scroll->viewport(), event);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace usdcc::ui
