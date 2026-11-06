#pragma once

#include "usdcc/core/export.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTreeWidget;

namespace usdcc::usd {

// Debug > Shader Nodes: everything ShaderNodeCatalog offers, grouped as the
// Material Editor's palette will be, with a selected node's inputs and outputs.
class USDCC_USD_UI_API ShaderNodesDialog : public QDialog {
    Q_OBJECT

public:
    explicit ShaderNodesDialog(QWidget* parent = nullptr);

    QTreeWidget* tree() const { return m_tree; }
    QLineEdit* search() const { return m_search; }
    QPlainTextEdit* details() const { return m_details; }

private:
    void rebuild();
    void applySearch(const QString& text);
    void showDetails();

    QLabel* m_summary = nullptr;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_details = nullptr;
};

}  // namespace usdcc::usd
