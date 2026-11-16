#pragma once

// Linear images to OpenEXR (docs/COLOR_MANAGEMENT.md §6): what a render view panel's
// Save Image writes for "OpenEXR, linear", the renderer's working-space values with no view
// transform. OpenEXR comes from the OpenUSD install, as OpenColorIO does; built without it,
// linearExrAvailable() is false and nothing is written. Qt-free.

#include <string>

namespace usdcc::usd {

// Whether usdcc was built with OpenEXR.
bool linearExrAvailable();

// Writes `rgba` (width × height pixels of R, G, B, A floats, top row first, `rowFloats`
// floats from one row to the next; 0 for width × 4) to `path`: float R, G, B and A channels,
// ZIP-compressed, with the working space's chromaticities (Rec.709 primaries, D65) in the
// header. False, with `error` saying why, when it can't.
bool writeLinearExr(const std::string& path, const float* rgba, int width, int height, std::string* error = nullptr,
                    size_t rowFloats = 0);

}  // namespace usdcc::usd
