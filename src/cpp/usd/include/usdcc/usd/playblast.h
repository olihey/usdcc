#pragma once

// A viewport playblast (docs/ANIMATION.md phase 5): the viewport's picture at each frame of a
// range, written as an image sequence. Each frame is drawn at its time and grabbed once the
// renderer has converged (Storm at once, a path tracer when it's done), as grabFrame() has
// it: the scene with the grid and the selection outline, without the HUD.

#include "usdcc/core/export.h"

#include "usdcc/usd/time_range.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace usdcc::usd {

class HydraViewportWindow;
class Stage;

struct PlayblastOptions {
    TimeRange frames{1.0, 1.0};
    double step = 1.0;
    // Where each frame goes, with its number: "<frame>", "####", "$F4" or "$F" (four digits),
    // else ".<frame>" before the extension. PNG, JPEG or TIFF, by the extension.
    QString pattern;
    // Longest per frame for a renderer to converge before the frame is taken as it is (ms).
    int convergeTimeoutMs = 30000;
};

// `pattern` with `frame`'s number in it, as PlayblastOptions::pattern says.
USDCC_USD_UI_API QString playblastFile(const QString& pattern, double frame);
// The frames of the range, `step` apart, the last one included.
USDCC_USD_UI_API std::vector<double> playblastFrames(const PlayblastOptions& options);

// Plays the stage through `options.frames` in `window` and writes each picture. `progress`
// is told after each frame (frames done, frames in all) and stops it by returning false. The
// stage's time is put back afterwards. Returns the files written; on a failure, the ones
// written so far, with `error` set.
USDCC_USD_UI_API QStringList playblast(HydraViewportWindow& window, Stage& stage, const PlayblastOptions& options,
                                       const std::function<bool(int done, int total)>& progress = {},
                                       QString* error = nullptr);

}  // namespace usdcc::usd
