// The viewport playblast's file names and frames (docs/ANIMATION.md phase 5). The playblast
// itself draws in a viewport on screen, which the offscreen tests don't have; it was run in the
// app. Runs in usdcc_ui_tests.
#include "test_harness.h"

#include "usdcc/usd/playblast.h"

#include <vector>

using namespace usdcc::usd;

USDCC_TEST(playblast_files_and_frames) {
    CHECK(playblastFile(QStringLiteral("C:/out/frame.####.png"), 12.0) == QStringLiteral("C:/out/frame.0012.png"), "####");
    CHECK(playblastFile(QStringLiteral("out/s_<frame>.jpg"), 7.0) == QStringLiteral("out/s_0007.jpg"), "<frame>");
    CHECK(playblastFile(QStringLiteral("out/s.$F4.tif"), 120.0) == QStringLiteral("out/s.0120.tif"), "$F4");
    CHECK(playblastFile(QStringLiteral("out/shot.png"), 3.0) == QStringLiteral("out/shot.0003.png"),
          "no token: the frame before the extension");
    CHECK(playblastFile(QStringLiteral("out.v2/shot"), 3.0) == QStringLiteral("out.v2/shot.0003"),
          "a dot in a folder isn't an extension");
    CHECK(playblastFile(QStringLiteral("f.####.png"), 12.5) == QStringLiteral("f.0012.5.png"), "a fraction kept");
    CHECK(playblastFile(QStringLiteral("f.####.png"), -3.0) == QStringLiteral("f.-0003.png"), "negative frames");

    PlayblastOptions options;
    options.frames = {1.0, 6.0};
    options.step = 2.0;
    CHECK(playblastFrames(options) == std::vector<double>({1.0, 3.0, 5.0}), "every second frame");
    options.frames = {10.0, 8.0};
    options.step = 1.0;
    CHECK(playblastFrames(options) == std::vector<double>({8.0, 9.0, 10.0}), "a range the wrong way round");
}
