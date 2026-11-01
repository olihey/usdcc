#pragma once

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QDialog>

class QCheckBox;
class QLabel;
class QListWidget;
class QStackedWidget;

namespace usdcc::usd::properties {

// Debug > USD Data Types (docs/ATTRIBUTE_EDITOR.md §8): every value type this
// USD build registers, each shown as a real PropertyRow bound to an attribute
// on a private in-memory stage, so all editors can be tried and compared in
// one place. Toggles preview the row states (read-only, unauthored, animated,
// connected) and soft limits (sliders); a log line shows the edit signals as
// they happen, and the footer counts types without a dedicated editor.
class DataTypeGalleryDialog : public QDialog {
    Q_OBJECT

public:
    explicit DataTypeGalleryDialog(QWidget* parent = nullptr);

private:
    void buildStage();
    void applyLimits(bool on);
    void applyAuthored(bool authored);
    void rebuild();
    void log(const QString& text);

    PXR_NS::UsdStageRefPtr m_stage;
    PXR_NS::UsdPrim m_prim;
    QListWidget* m_groups;
    QStackedWidget* m_pages;
    QCheckBox* m_readOnly;
    QCheckBox* m_unauthored;
    QCheckBox* m_animated;
    QCheckBox* m_connected;
    QCheckBox* m_limits;
    QLabel* m_log;
    QLabel* m_footer;
    int m_lastTab = 0;
};

}  // namespace usdcc::usd::properties
