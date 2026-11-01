#pragma once

#include <QString>
#include <QWidget>

class QLabel;

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

    void setUnauthored(bool unauthored);
    void setAnimated(bool animated);
    void setConnectedSource(const QString& source);  // empty = not connected
    void setReadOnly(bool readOnly);                  // forced, independent of the states above
    void setResettable(bool resettable) { m_resettable = resettable; }

Q_SIGNALS:
    // "Reset to Default" from the context menu: clear the authored value.
    void resetRequested();

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void updateReadOnly();
    void updateBadge();

    PropertyEditor* m_editor;
    QLabel* m_label;
    QLabel* m_note;
    QLabel* m_badge;
    QLabel* m_subLine;
    QString m_propertyPath;
    QString m_connectedSource;
    bool m_animated = false;
    bool m_readOnly = false;
    bool m_resettable = false;
};

}  // namespace usdcc::usd::properties
