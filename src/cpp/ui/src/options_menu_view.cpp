#include "usdcc/ui/options_menu_view.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

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

}  // namespace usdcc::ui
