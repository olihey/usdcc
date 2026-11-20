#pragma once

// Remote assets in the app's UI (docs/REMOTE_ASSETS.md §10): Settings > Assets, the
// login dialog, Open Stage from URL, and what the library does shown in the window.

#include "usdcc/core/export.h"
#include "usdcc/core/settings.h"

#include <QString>

class QMainWindow;
class QWidget;

namespace usdcc::usd {

inline constexpr const char* kAssetEnvironmentKey = "Assets/environment";
inline constexpr const char* kAssetOfflineKey = "Assets/offline";
inline constexpr const char* kAssetCacheLimitKey = "Assets/cacheLimitGB";
inline constexpr const char* kAssetHttpHostsKey = "Assets/httpHosts";
inline constexpr const char* kAssetS3BucketsKey = "Assets/s3Buckets";
inline constexpr const char* kAssetClearCacheKey = "Assets/clearCache";  // a button

// Defines Settings > Assets and applies it to the asset library, now and whenever one
// changes. Before the first stage opens.
USDCC_USD_UI_API void defineAssetSettings(core::Settings& settings = core::Settings::instance());

// Downloads in `window`'s status bar while they run, and a toast when a stage asks for
// an HTTP host that isn't allowed. Once, when the window exists.
USDCC_USD_UI_API void installAssetActivity(QMainWindow* window);

// File > Open Stage from URL...: a webdav:// or https:// path, typed or pasted. Empty
// when cancelled.
USDCC_USD_UI_API QString askForAssetUrl(QWidget* parent);

// A path to show: a file's with the system's separators, a server's (webdav://,
// https://) as it is, and without the stack id a resolved one carries.
USDCC_USD_UI_API QString displayPath(const QString& path);

// Edit > Asset Server Login...: a credential id's user and password, or token, into
// the system's credential store (Windows Credential Manager). With `credential`, that
// one is filled in; with `scheme` "s3", it's an access key and its secret. True when
// one was saved.
USDCC_USD_UI_API bool showAssetLoginDialog(QWidget* parent, const QString& credential = {}, const QString& scheme = {});

}  // namespace usdcc::usd
