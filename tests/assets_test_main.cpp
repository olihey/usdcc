#include "test_harness.h"

#include <pxr/base/plug/registry.h>

int main(int argc, char** argv) {
    // Before anything asks OpenUSD for its resolver, which collects the URI resolvers
    // once: the fake:// protocol the tests resolve through (assets_fake_protocol.cpp).
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_ASSETS_FAKE_PROTOCOL_DIR);
    // And the real ones (assets_net_test.cpp), as the app's plugin scan registers them.
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_ASSETS_HTTP_PROTOCOL_DIR);
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_ASSETS_WEBDAV_PROTOCOL_DIR);
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_ASSETS_S3_PROTOCOL_DIR);
    return usdcc::test::runTests(argc, argv);
}
