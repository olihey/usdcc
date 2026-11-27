#pragma once

// Remote assets in the app (docs/REMOTE_ASSETS.md §10): the resolver context each
// stage is opened with, and what the UI shows of a path on a server. Qt-free, like the
// rest of usdcc_usd. The library underneath is usdcc_assets.

#include "usdcc/core/signal.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/usd/stage.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// The environments the asset configuration names (docs/REMOTE_ASSETS.md §4), sorted.
std::vector<std::string> assetEnvironments();

// Whether the configuration has it.
bool hasAssetEnvironment(const std::string& environment);
// After the Asset Servers editor wrote the configuration. On the UI thread.
usdcc::core::Signal<>& assetConfigChanged();

// The environment a stage opens in when none is asked for: Settings > Assets' when the
// configuration has it, else the configuration's own choice (USDCC_ASSET_ENV, or its
// "environment"). Empty when there is none: each server is then a name by itself
// (docs/REMOTE_ASSETS.md §4.1).
std::string defaultAssetEnvironment();
// Settings > Assets' choice; empty for the configuration's. Set by the app from its setting.
void setPreferredAssetEnvironment(const std::string& environment);

// The environment chosen for a folder's stages (docs/ASSET_SOURCES.md §4), kept in `file`
// (JSON). Without a file, as in tools and tests, nothing is remembered.
void setAssetFolderFile(const std::string& file);
void rememberFolderEnvironment(const std::string& folder, const std::string& environment);
// The one remembered for the folder of `identifier`, else for the nearest above it; empty for
// none. One the configuration doesn't have any more doesn't count.
std::string rememberedEnvironment(const std::string& identifier);
// Forgets that one. False when there was none.
bool forgetFolderEnvironment(const std::string& identifier);
// The folder of a stage's root, as rememberFolderEnvironment() takes it.
std::string folderOf(const std::string& identifier);

// The context to open `identifier` with: the environment's servers (empty: the one
// remembered for its folder, else the default one), and whatever OpenUSD's own resolvers give the asset, its folder for one.
PXR_NS::ArResolverContext stageResolverContext(const std::string& identifier, const std::string& environment = {});

// The environment a stage was opened in; empty when it has none (one in memory, or one
// opened elsewhere without usdcc's context).
std::string stageAssetEnvironment(const PXR_NS::UsdStageRefPtr& stage);

// A path on disk, or anything else that isn't a URI ("webdav://", "https://"). A path
// inside a package ("a.usdz[tex.png]") counts as local.
bool isLocalAssetPath(const std::string& path);

// A resolved path as the UI shows it: one on a server without the stack id it carries
// (webdav://studio~7c21/a.usd -> webdav://studio/a.usd), anything else as it is.
std::string displayAssetPath(const std::string& resolvedPath);

// `target` (a server's file) written relative to the layer at `anchor` (that layer's
// path or resolved path): "../chars/hero.usd" from webdav://studio/shots/s.usd to
// webdav://studio/chars/hero.usd. Empty when there is none: different schemes or servers,
// either isn't on a server, or the layer is a package or in one (.usdz), where a relative path
// is anchored inside it (docs/ASSET_PICKER.md §4).
std::string relativeAssetPath(const std::string& target, const std::string& anchor);

// Where an anchored asset path points, without waiting for a server (usdcc/assets/no_wait.h),
// under the context that is bound. A layer that's open counts as found, though the resolver may
// know no file for it.
struct LocatedAsset {
    std::string path;        // as the UI shows it; empty when it isn't found
    std::string origin;      // assetOrigin()
    bool resolving = false;  // no server answered yet: not found, so far
};
LocatedAsset locateAsset(const std::string& anchored);

// The server that delivered a resolved asset: an endpoint's id ("test", "prod") for a
// named protocol, the host for plain HTTP. Empty for a local file.
std::string assetOrigin(const std::string& assetPath, const std::string& resolvedPath);

}  // namespace usdcc::usd
