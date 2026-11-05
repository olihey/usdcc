#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/preview_stage.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/usda_view.h"

#include <QDialog>
#include <QString>

namespace usdcc::usd {

// Show USDA (docs/USDA_VIEW.md §10): a quick look at a file as USDA text, in a
// non-modal dialog with everything the USDA panel has (a UsdaView). It has a
// stage of its own, which StageManager never hears of: not in the Stages menu,
// not current, no undo, gone when the dialog closes. Layers are shared, as USD
// keeps one per file, so a layer's unsaved edits show here.
//
// The file's text shows at once. The stage loads behind it on a worker thread
// (without payloads; Load Payloads is in the options menu), and adds the layer
// stack and the flattened views to Show when it's ready. Closing the dialog
// meanwhile just drops the result.
class USDCC_USD_UI_API UsdaPreviewDialog : public QDialog {
    Q_OBJECT

public:
    // `path`: a file, or an open layer's identifier.
    explicit UsdaPreviewDialog(const QString& path, QWidget* parent = nullptr);

    const QString& path() const { return m_path; }
    UsdaView* view() const { return m_view; }
    // False until the stage has loaded, or failed to.
    bool isStageLoaded() const { return m_loaded; }

public Q_SLOTS:
    // Esc: the Find bar first, then the dialog.
    void reject() override;
    void done(int result) override;

private:
    void startLoading();
    void populateOptionsMenu(QMenu* menu);

    QString m_path;
    UsdaView* m_view;
    bool m_loaded = false;
};

// Shows `path` in a preview dialog over `parent`'s window, or brings forward
// the one already showing it. Null if it can't be shown.
UsdaPreviewDialog* showUsdaPreview(const QString& path, QWidget* parent);

}  // namespace usdcc::usd
