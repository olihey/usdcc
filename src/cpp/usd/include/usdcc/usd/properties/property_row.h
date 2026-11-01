#pragma once

#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QMenu;

namespace usdcc::usd::properties {

class PropertyEditor;

// One line of the attribute editor (docs/ATTRIBUTE_EDITOR.md §4.1): the label
// on the left, the value editor on the right, plus state badges and a context
// menu. The row owns the editor.
//
// States, each shown and each making the editor read-only where an edit
// would be wrong or pointless:
//   unauthored — the value is a schema fallback, nothing authored (dimmed label)
//   animated   — the attribute has time samples; with no timeline to pick a
//                time, editing is disabled
//   connected  — the value comes from a connection; shows the source
class PropertyRow : public QWidget {
    Q_OBJECT

public:
    PropertyRow(const QString& label, PropertyEditor* editor, QWidget* parent = nullptr);

    PropertyEditor* editor() const { return m_editor; }

    void setLabelWidth(int width);
    void setLabelToolTip(const QString& toolTip);
    // Muted text after the editor (the data-type gallery shows the C++ type
    // there). A fixed width keeps the editors of neighbouring rows aligned.
    void setNote(const QString& note, int fixedWidth = 0);
    // Full property path, for the context menu's "Copy Property Path".
    void setPropertyPath(const QString& path) { m_propertyPath = path; }
    // An extra control after the editor (e.g. a relationship's "Select" button).
    void addTrailingWidget(QWidget* widget);
    // An extra control before the label (e.g. a transform op's drag handle).
    void addLeadingWidget(QWidget* widget);
    QString labelText() const;

    // Marks the row as the tool target (the transform stack's selected op).
    void setTargeted(bool targeted);
    // Adds entries to the row's context menu (after the standard ones).
    void setExtraMenu(std::function<void(QMenu*)> populate) { m_extraMenu = std::move(populate); }

    void setUnauthored(bool unauthored);
    void setAnimated(bool animated);
    void setConnectedSource(const QString& source);  // empty = not connected
    void setReadOnly(bool readOnly);                  // forced, independent of the states above
    void setResettable(bool resettable) { m_resettable = resettable; }

Q_SIGNALS:
    // "Reset to Default" from the context menu: clear the authored value.
    void resetRequested();
    // The label was clicked.
    void labelClicked();

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateReadOnly();
    void updateBadge();

    PropertyEditor* m_editor;
    QLabel* m_label;
    QLabel* m_note;
    QLabel* m_badge;
    QLabel* m_subLine;
    QWidget* m_valueColumn;
    QString m_propertyPath;
    QString m_connectedSource;
    bool m_animated = false;
    bool m_readOnly = false;
    bool m_resettable = false;
    QString m_labelText;
    std::function<void(QMenu*)> m_extraMenu;
};

}  // namespace usdcc::usd::properties
