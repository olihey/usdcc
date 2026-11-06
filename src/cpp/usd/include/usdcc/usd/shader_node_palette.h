#pragma once

#include "usdcc/core/export.h"

#include <QFrame>
#include <QString>
#include <QTreeWidget>
#include <QWidget>

#include <string>
#include <vector>

class QLineEdit;

namespace usdcc::usd {

// The shader nodes to choose from, by shading system, context and group, with a search over
// label, function and identifier (docs/MATERIAL_EDITOR.md §3.2). Choosing one, or dragging it
// onto the graph, adds it.
class USDCC_USD_UI_API ShaderNodePalette : public QWidget {
    Q_OBJECT

public:
    // Dragged out as this, holding the identifier.
    static constexpr auto kMimeType = "application/x-usdcc-shader-node";

    explicit ShaderNodePalette(QWidget* parent = nullptr);

    // These come first and start open: what the graph is using.
    void setPreferredSystems(const std::vector<std::string>& systems);
    // Reads the catalog again, as a plugin may have added to it.
    void refresh();

    QLineEdit* search() const { return m_search; }
    QTreeWidget* tree() const { return m_tree; }
    // What Return would choose.
    QString firstMatch() const;

Q_SIGNALS:
    void nodeChosen(const QString& identifier);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applySearch(const QString& text);

    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    std::vector<std::string> m_preferred;
};

// The same in a popup at the mouse, for Tab: type, Return, gone.
class USDCC_USD_UI_API ShaderNodeSearchPopup : public QFrame {
    Q_OBJECT

public:
    explicit ShaderNodeSearchPopup(QWidget* parent = nullptr);
    ShaderNodePalette* palette() const { return m_palette; }

Q_SIGNALS:
    void nodeChosen(const QString& identifier);

private:
    ShaderNodePalette* m_palette;
};

}  // namespace usdcc::usd
