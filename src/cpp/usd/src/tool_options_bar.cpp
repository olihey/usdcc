#include "usdcc/usd/tool_options_bar.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QToolButton>

namespace usdcc::usd {

using usdcc::tools::OptionValue;
using usdcc::tools::ToolOption;

namespace {
constexpr int kBarHeight = 30;  // the design's options-bar height
constexpr int kNumberWidth = 72;

// A label followed by a control, as one widget so the pair can be enabled,
// disabled and removed together.
QWidget* labelled(const QString& label, QWidget* control, QWidget* parent) {
    auto* group = new QWidget(parent);
    auto* layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    usdcc::ui::scaled::spacing(layout, 6);
    if (!label.isEmpty()) {
        layout->addWidget(new QLabel(label, group));
    }
    control->setParent(group);
    layout->addWidget(control);
    return group;
}
}  // namespace

ToolOptionsBar::ToolOptionsBar(QWidget* parent) : QFrame(parent), m_layout(new QHBoxLayout(this)) {
    setObjectName(QStringLiteral("toolOptionsBar"));
    setAttribute(Qt::WA_StyledBackground);
    usdcc::ui::scaled::fixedHeight(this, kBarHeight);
    // Fill the whole width of the QToolBar that hosts it.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    usdcc::ui::scaled::margins(m_layout, 10, 0, 10, 0);
    usdcc::ui::scaled::spacing(m_layout, 12);
}

void ToolOptionsBar::clear() {
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();  // gone from the layout now; don't let it linger until deleteLater() runs
            widget->deleteLater();
        }
        delete item;
    }
}

void ToolOptionsBar::addSeparator() {
    auto* line = new QFrame(this);
    line->setObjectName(QStringLiteral("toolOptionsSeparator"));
    m_layout->addWidget(line);
}

void ToolOptionsBar::setTool(const QString& toolName, const std::vector<ToolOption>& options) {
    clear();

    auto* title = new QLabel(toolName, this);
    title->setObjectName(QStringLiteral("toolOptionsTitle"));
    m_layout->addWidget(title);

    QHash<QString, QCheckBox*> toggles;
    struct Dependent {
        QWidget* widget;
        QString enabledBy;
    };
    std::vector<Dependent> dependents;

    for (const ToolOption& option : options) {
        addSeparator();
        QWidget* control = nullptr;
        switch (option.type) {
            case ToolOption::Type::Choice:
                control = makeChoice(option);
                break;
            case ToolOption::Type::Toggle: {
                control = makeToggle(option);
                toggles.insert(QString::fromStdString(option.id), static_cast<QCheckBox*>(control));
                break;
            }
            case ToolOption::Type::Number:
                control = makeNumber(option);
                break;
        }
        m_layout->addWidget(control);
        if (!option.enabledBy.empty()) {
            dependents.push_back({control, QString::fromStdString(option.enabledBy)});
        }
    }

    // An option that only makes sense while a toggle is on (e.g. the snap
    // increment) follows that toggle.
    for (const Dependent& dependent : dependents) {
        QCheckBox* toggle = toggles.value(dependent.enabledBy, nullptr);
        if (!toggle) {
            continue;
        }
        dependent.widget->setEnabled(toggle->isChecked());
        connect(toggle, &QCheckBox::toggled, dependent.widget, &QWidget::setEnabled);
    }

    m_layout->addStretch(1);
}

QWidget* ToolOptionsBar::makeChoice(const ToolOption& option) {
    auto* segmented = new QFrame();
    segmented->setObjectName(QStringLiteral("segmented"));
    auto* layout = new QHBoxLayout(segmented);
    usdcc::ui::scaled::margins(layout, 2, 2, 2, 2);
    usdcc::ui::scaled::spacing(layout, 2);

    auto* group = new QButtonGroup(segmented);
    group->setExclusive(true);
    const std::string selected = std::get<std::string>(option.value);
    for (const ToolOption::Choice& choice : option.choices) {
        auto* button = new QToolButton(segmented);
        button->setObjectName(QStringLiteral("segmentButton"));
        button->setText(QString::fromStdString(choice.label));
        button->setCheckable(true);
        button->setChecked(choice.id == selected);
        button->setFocusPolicy(Qt::NoFocus);
        group->addButton(button);
        layout->addWidget(button);
        connect(button, &QToolButton::clicked, this,
                [this, id = option.id, choiceId = choice.id]() { Q_EMIT optionChanged(id, OptionValue(choiceId)); });
    }
    return labelled(QString::fromStdString(option.label), segmented, this);
}

QWidget* ToolOptionsBar::makeToggle(const ToolOption& option) {
    // The label lives in the checkbox itself, as in the design ("Select Through").
    auto* box = new QCheckBox(QString::fromStdString(option.label), this);
    box->setChecked(std::get<bool>(option.value));
    box->setFocusPolicy(Qt::NoFocus);
    connect(box, &QCheckBox::toggled, this,
            [this, id = option.id](bool on) { Q_EMIT optionChanged(id, OptionValue(on)); });
    return box;
}

QWidget* ToolOptionsBar::makeNumber(const ToolOption& option) {
    auto* spin = new QDoubleSpinBox();
    spin->setRange(option.min, option.max);
    spin->setDecimals(option.decimals);
    spin->setSingleStep(option.step);
    spin->setSuffix(QString::fromUtf8(option.suffix.data(), static_cast<qsizetype>(option.suffix.size())));
    spin->setValue(std::get<double>(option.value));
    usdcc::ui::scaled::fixedWidth(spin, kNumberWidth);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    // Report the value once it's committed (Enter / focus out), not on every
    // keystroke — typing "0.25" must not pass through 0, 0.2 along the way.
    spin->setKeyboardTracking(false);
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            [this, id = option.id](double value) { Q_EMIT optionChanged(id, OptionValue(value)); });
    return labelled(QString::fromStdString(option.label), spin, this);
}

}  // namespace usdcc::usd
