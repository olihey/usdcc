#pragma once

#include <QKeyEvent>
#include <QLineEdit>

#include <functional>

namespace usdcc::ui {

// A find box: Enter for the next match, Shift+Enter the previous one, Esc closes. What the
// USDA view and the Material Editor put in their find bars.
class FindLineEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void(bool backward)> onEnter;
    std::function<void()> onEscape;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && onEnter) {
            onEnter(event->modifiers() & Qt::ShiftModifier);
            return;
        }
        if (event->key() == Qt::Key_Escape && onEscape) {
            onEscape();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

}  // namespace usdcc::ui
