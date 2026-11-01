#include "usdcc/usd/properties/property_row.h"

#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_style.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QStyle>
#include <QVBoxLayout>

namespace usdcc::usd::properties {

namespace {
void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
}  // namespace

PropertyRow::PropertyRow(const QString& label, PropertyEditor* editor, QWidget* parent)
    : QWidget(parent), m_editor(editor) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_label = new QLabel(label, this);
    m_label->setObjectName(QStringLiteral("propertyLabel"));
    m_label->setFixedHeight(style::kFieldHeight);
    m_label->setToolTip(label);
    layout->addWidget(m_label, 0, Qt::AlignTop);

    auto* valueColumn = new QWidget(this);
    auto* valueLayout = new QVBoxLayout(valueColumn);
    valueLayout->setContentsMargins(0, 0, 0, 0);
    valueLayout->setSpacing(2);
    editor->setParent(valueColumn);
    valueLayout->addWidget(editor);
    m_subLine = new QLabel(valueColumn);
    m_subLine->setObjectName(QStringLiteral("propertySubLine"));
    m_subLine->setVisible(false);
    valueLayout->addWidget(m_subLine);
    layout->addWidget(valueColumn, 1);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("propertyNote"));
    m_note->setFixedHeight(style::kFieldHeight);
    m_note->setVisible(false);
    layout->addWidget(m_note, 0, Qt::AlignTop);

    m_badge = new QLabel(this);
    m_badge->setObjectName(QStringLiteral("propertyBadge"));
    m_badge->setFixedHeight(style::kFieldHeight);
    m_badge->setVisible(false);
    layout->addWidget(m_badge, 0, Qt::AlignTop);

    setLabelWidth(110);
}

void PropertyRow::setLabelWidth(int width) { m_label->setFixedWidth(width); }

void PropertyRow::addTrailingWidget(QWidget* widget) {
    auto* rowLayout = static_cast<QHBoxLayout*>(layout());
    // Before the note and badge, right after the value column.
    rowLayout->insertWidget(2, widget, 0, Qt::AlignTop);
}

QString PropertyRow::labelText() const { return m_label->text(); }

void PropertyRow::setLabelToolTip(const QString& toolTip) { m_label->setToolTip(toolTip); }

void PropertyRow::setNote(const QString& note, int fixedWidth) {
    m_note->setText(note);
    m_note->setToolTip(note);
    m_note->setVisible(!note.isEmpty());
    if (fixedWidth > 0) {
        m_note->setFixedWidth(fixedWidth);
    }
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

void PropertyRow::setConnectedSource(const QString& source) {
    m_connectedSource = source;
    m_subLine->setText(source.isEmpty() ? QString() : tr("Connected \xC2\xB7 %1").arg(source));
    m_subLine->setVisible(!source.isEmpty());
    updateBadge();
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
    if (m_animated) {
        text = tr("animated");
        tip = tr("This attribute has time samples. Editing them needs a timeline, which usdcc doesn't have yet.");
    } else if (!m_connectedSource.isEmpty()) {
        text = tr("connected");
        tip = tr("The value comes from %1.").arg(m_connectedSource);
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
    menu.exec(event->globalPos());
}

}  // namespace usdcc::usd::properties
