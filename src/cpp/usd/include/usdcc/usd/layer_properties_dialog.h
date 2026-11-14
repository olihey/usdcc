#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/layer_actions.h"

#include <pxr/usd/sdf/layer.h>

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace usdcc::usd {

// Edit Layer Properties (docs/LAYER_EDITOR.md §6), from the pencil on the
// Layer Editor's Layer Properties header: every property of a layer that can
// be changed, filled from `current`. An empty field is unset: USD's fallback,
// shown as its placeholder. Time Offset and Time Scale are there for a
// sublayer only. Nothing is authored here: layer_actions::setLayerProperties()
// authors values(). OK is off while a value can't be used (a bad prim name, a
// rate or units not above 0, a scale of 0); an end before the start is only a
// warning, as USD allows it.
class USDCC_USD_UI_API LayerPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    LayerPropertiesDialog(const PXR_NS::SdfLayerHandle& layer, const layer_actions::LayerProperties& current,
                          bool sublayer, QWidget* parent = nullptr);

    layer_actions::LayerProperties values() const;

    QLineEdit* defaultPrimField() const { return m_defaultPrim; }
    // Right of Default Prim: a picker of the layer's prims (the layer composed on
    // its own, in Show Prims' tree) that fills the field: a root prim's name, or
    // another prim's path.
    QToolButton* pickDefaultPrimButton() const { return m_pickDefaultPrim; }
    QComboBox* upAxisCombo() const { return m_upAxis; }
    QComboBox* metersPerUnitCombo() const { return m_metersPerUnit; }
    QLineEdit* startTimeCodeField() const { return m_start; }
    QLineEdit* endTimeCodeField() const { return m_end; }
    QLineEdit* timeCodesPerSecondField() const { return m_timeCodesPerSecond; }
    QLineEdit* framesPerSecondField() const { return m_framesPerSecond; }
    QLineEdit* timeOffsetField() const { return m_offset; }  // null for a layer that isn't a sublayer
    QLineEdit* timeScaleField() const { return m_scale; }    // likewise
    QLineEdit* commentField() const { return m_comment; }
    QPlainTextEdit* documentationField() const { return m_documentation; }
    QPushButton* okButton() const { return m_ok; }
    QLabel* problemLabel() const { return m_problem; }

private:
    void check();
    void pickDefaultPrim();

    PXR_NS::SdfLayerHandle m_layer;
    QLineEdit* m_defaultPrim;
    QToolButton* m_pickDefaultPrim;
    QComboBox* m_upAxis;
    QComboBox* m_metersPerUnit;
    QLineEdit* m_start;
    QLineEdit* m_end;
    QLineEdit* m_timeCodesPerSecond;
    QLineEdit* m_framesPerSecond;
    QLineEdit* m_offset = nullptr;
    QLineEdit* m_scale = nullptr;
    QLineEdit* m_comment;
    QPlainTextEdit* m_documentation;
    QPushButton* m_ok;
    QLabel* m_problem;
};

}  // namespace usdcc::usd
