#include "test_harness.h"

#include <pxr/base/plug/registry.h>

#include <QApplication>

int main(int argc, char** argv) {
    // Widgets need a full QApplication; CTest runs this on Qt's offscreen
    // platform (see CMakeLists.txt), so no window ever appears.
    QApplication app(argc, argv);
#ifdef USDCC_UI_TESTS_WEBDAV_PROTOCOL_DIR
    // Before anything asks for the resolver, which finds its URI resolvers once: webdav://
    // for the asset picker's save tests (asset_picker_save_test.cpp).
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_UI_TESTS_WEBDAV_PROTOCOL_DIR);
#endif
    return usdcc::test::runTests(argc, argv);
}
