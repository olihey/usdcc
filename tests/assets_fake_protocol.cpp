// A protocol plugin for the tests of usdcc_assets (docs/REMOTE_ASSETS.md §14): the
// resolver for fake://. It is all a protocol's resolver has to be. Its transport,
// over folders on disk, lives in the test program (assets_test.cpp), which registers
// it for the scheme and holds its switches.
#include "usdcc/assets/caching_resolver.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/defineResolver.h>

PXR_NAMESPACE_OPEN_SCOPE

class UsdccFakeResolver final : public usdcc::assets::CachingResolver {
public:
    UsdccFakeResolver() : CachingResolver("fake") {}
};

AR_DEFINE_RESOLVER(UsdccFakeResolver, ArResolver);

PXR_NAMESPACE_CLOSE_SCOPE
