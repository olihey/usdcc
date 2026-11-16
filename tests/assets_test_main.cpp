#include "test_harness.h"

#include <pxr/base/plug/registry.h>

int main(int argc, char** argv) {
    // Before anything asks OpenUSD for its resolver, which collects the URI resolvers
    // once: the fake:// protocol the tests resolve through (assets_fake_protocol.cpp).
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_ASSETS_FAKE_PROTOCOL_DIR);
    return usdcc::test::runTests(argc, argv);
}
