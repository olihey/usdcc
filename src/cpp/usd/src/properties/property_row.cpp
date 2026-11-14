#include "usdcc/usd/properties/property_row.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_style.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace usdcc::usd::properties {

namespace {
void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
}  // namespace

QToolButton* iconButton(const QString& iconPath, const QString& toolTip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("propertyIconButton"));  // styled by theme/dark.qss
    if (!iconPath.isEmpty()) button->setIcon(QIcon(iconPath));
    button->setToolTip(toolTip);
    usdcc::ui::scaled::fixedSize(button, style::kFieldHeight, style::kFieldHeight);
    usdcc::ui::scaled::iconSize(button, 12, 12);
    return button;
}

PropertyRow::PropertyRow(const QString& label, PropertyEditor* editor, QWidget* parent)
    : QWidget(parent), m_editor(editor) {
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    usdcc::ui::scaled::spacing(rows, 2);
    auto* layout = new QHBoxLayout();
    m_rowLayout = layout;
    usdcc::ui::scaled::spacing(layout, 8);
    rows->addLayout(layout);
    // Empty (all hidden) unless there's a second line, so it adds no height.
    m_subLayout = new QHBoxLayout();
    m_subLayout->setSpacing(0);
    rows->addLayout(m_subLayout);

    m_labelText = label;
    m_label = new QLabel(label, this);
    m_label->setObjectName(QStringLiteral("propertyLabel"));
    usdcc::ui::scaled::fixedHeight(m_label, style::kFieldHeight);
    m_label->setToolTip(label);
    m_label->installEventFilter(this);
    layout->addWidget(m_label, 0, Qt::AlignTop);

    auto* valueColumn = new QWidget(this);
    m_valueColumn = valueColumn;
    auto* valueLayout = new QVBoxLayout(valueColumn);
    valueLayout->setContentsMargins(0, 0, 0, 0);
    editor->setParent(valueColumn);
    valueLayout->addWidget(editor);
    layout->addWidget(valueColumn, 1);
    // Follows the value column's left edge (see eventFilter()).
    valueColumn->installEventFilter(this);

    m_sourceLink = new QToolButton(this);
    m_sourceLink->setObjectName(QStringLiteral("subLineLink"));  // styled by theme/dark.qss
    m_sourceLink->setCursor(Qt::PointingHandCursor);
    m_sourceLink->setMinimumWidth(1);  // a long path is cut short rather than widening the row
    m_sourceLink->setVisible(false);
    connect(m_sourceLink, &QToolButton::clicked, this, &PropertyRow::connectedSourceClicked);
    m_subLayout->addWidget(m_sourceLink, 0, Qt::AlignLeft);

    m_subLine = new QLabel(this);
    m_subLine->setObjectName(QStringLiteral("propertySubLine"));
    m_subLine->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // never widens the row
    m_subLine->setVisible(false);
    m_subLayout->addWidget(m_subLine, 1);
    if (QWidget* subLine = editor->subLineWidget()) {
        if (QLayout* editorLayout = subLine->parentWidget() ? subLine->parentWidget()->layout() : nullptr) {
            editorLayout->removeWidget(subLine);
        }
        subLine->setParent(this);
        m_subLayout->addWidget(subLine, 1);
    }

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("propertyNote"));
    usdcc::ui::scaled::fixedHeight(m_note, style::kFieldHeight);
    m_note->setVisible(false);
    layout->addWidget(m_note, 0, Qt::AlignTop);

    m_badge = new QLabel(this);
    m_badge->setObjectName(QStringLiteral("propertyBadge"));
    usdcc::ui::scaled::fixedHeight(m_badge, style::kFieldHeight);
    m_badge->setVisible(false);
    layout->addWidget(m_badge, 0, Qt::AlignTop);

    // Last of all: addTrailingWidget() goes before the note, so this stays
    // the rightmost control.
    if (QWidget* button = editor->rowButton()) {
        if (QLayout* editorLayout = button->parentWidget() ? button->parentWidget()->layout() : nullptr) {
            editorLayout->removeWidget(button);
        }
        layout->addWidget(button, 0, Qt::AlignTop);
    }

    setLabelWidth(110);
}

void PropertyRow::setLabelWidth(int width) { usdcc::ui::scaled::fixedWidth(m_label, width); }

void PropertyRow::addTrailingWidget(QWidget* widget) {
    QHBoxLayout* rowLayout = m_rowLayout;
    // Right after the value column, before the note and badge.
    rowLayout->insertWidget(rowLayout->indexOf(m_valueColumn) + 1, widget, 0, Qt::AlignTop);
}

void PropertyRow::addLastWidget(QWidget* widget) {
    m_rowLayout->addWidget(widget, 0, Qt::AlignTop);
}

void PropertyRow::addLeadingWidget(QWidget* widget) {
    m_rowLayout->insertWidget(0, widget, 0, Qt::AlignTop);
}

QString PropertyRow::labelText() const { return m_labelText; }

void PropertyRow::setTargeted(bool targeted) {
    // "◆" marks the op the transform tools act on.
    m_label->setText(targeted ? m_labelText + QStringLiteral("  \xE2\x97\x86") : m_labelText);
    m_label->setProperty("target", targeted);
    repolish(m_label);
}

bool PropertyRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_label && event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        Q_EMIT labelClicked();
    }
    if (watched == m_valueColumn && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        // The second lines start where the value does.
        const int left = m_valueColumn->x();
        if (m_subLayout->contentsMargins().left() != left) m_subLayout->setContentsMargins(left, 0, 0, 0);
    }
    return QWidget::eventFilter(watched, event);
}

void PropertyRow::setLabelToolTip(const QString& toolTip) { m_label->setToolTip(toolTip); }

void PropertyRow::setNote(const QString& note, int fixedWidth, const QString& toolTip) {
    m_note->setText(note);
    m_note->setToolTip(toolTip.isEmpty() ? note : toolTip);
    m_note->setVisible(!note.isEmpty());
    if (fixedWidth > 0) {
        usdcc::ui::scaled::fixedWidth(m_note, fixedWidth);
    }
}

void PropertyRow::setSubLine(const QString& text, const QString& toolTip, SubLineTone tone) {
    m_subLine->setText(text);
    m_subLine->setToolTip(toolTip.isEmpty() ? text : toolTip);
    // The asset field's red, or the app's amber (theme/dark.qss).
    m_subLine->setProperty("unresolved", tone == SubLineTone::Error);
    m_subLine->setProperty("caution", tone == SubLineTone::Caution);
    repolish(m_subLine);
    m_subLine->setVisible(!text.isEmpty());
}

void PropertyRow::setUnused(bool unused) {
    if (!unused) {
        setGraphicsEffect(nullptr);  // deletes the effect
        return;
    }
    // An opacity effect rather than per-widget styling: it dims the label,
    // every kind of editor (several paint themselves) and the note alike.
    auto* effect = new QGraphicsOpacityEffect(this);
    effect->setOpacity(0.5);
    setGraphicsEffect(effect);
}

void PropertyRow::setUnauthored(bool unauthored) {
    m_label->setProperty("unauthored", unauthored);
    repolish(m_label);
}

void PropertyRow::setAnimated(bool animated) {
    m_animated = animated;
    updateBadge();
    updateReadOnly();
}

void PropertyRow::setConnectedSource(const QString& text, const QString& fullPath) {
    m_connectedSource = text;
    m_sourceLink->setText(text);
    m_sourceLink->setToolTip(text.isEmpty() ? QString()
                                            : tr("The value comes from %1. Click to select it.")
                                                  .arg(fullPath.isEmpty() ? text : fullPath));
    m_sourceLink->setVisible(!text.isEmpty());
    updateReadOnly();
}

void PropertyRow::setReadOnly(bool readOnly) {
    m_readOnly = readOnly;
    updateReadOnly();
}

void PropertyRow::updateReadOnly() {
    m_editor->setReadOnly(m_readOnly || m_animated || !m_connectedSource.isEmpty());
}

void PropertyRow::updateBadge() {
    QString text;
    QString tip;
    // A connection shows its source as a link under the value instead.
    if (m_animated) {
        text = tr("animated");
        tip = tr("This attribute has time samples. Editing them needs a timeline, which usdcc doesn't have yet.");
    }
    m_badge->setText(text);
    m_badge->setToolTip(tip);
    m_badge->setVisible(!text.isEmpty());
}

void PropertyRow::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    menu.addAction(tr("Copy Value"), this, [this]() {
        QApplication::clipboard()->setText(QString::fromStdString(formatValue(m_editor->value(), 6, 64)));
    });
    QAction* copyPath = menu.addAction(tr("Copy Property Path"), this,
                                       [this]() { QApplication::clipboard()->setText(m_propertyPath); });
    copyPath->setEnabled(!m_propertyPath.isEmpty());
    menu.addSeparator();
    QAction* reset = menu.addAction(tr("Reset to Default"), this, [this]() { Q_EMIT resetRequested(); });
    reset->setEnabled(m_resettable && !m_editor->isReadOnly());
    if (m_extraMenu) {
        menu.addSeparator();
        m_extraMenu(&menu);
    }
    menu.exec(event->globalPos());
}

}  // namespace usdcc::usd::properties
