# USD runtime

What the Help menu shows about the OpenUSD usdcc runs with, and the Settings > USD page for
the few things OpenUSD only takes from environment variables.

## 1. The Help menu

```
Help
  USD Runtime...
  ─────────────
  Project Home
  About usdcc
```

- **USD Runtime...** opens the dialog in §2.
- **Project Home** opens the GitHub repository (`AboutDialog::kRepositoryUrl`) in the
  system browser, with `QDesktopServices::openUrl`.
- **About usdcc** is docs/RELEASE.md §3.

## 2. The USD Runtime dialog

`usdcc::usd::UsdRuntimeDialog` (`usdcc_usd_ui`). It isn't modal, so debug flags can be
changed while working, and there is only one: `showDialog()` brings the open one to the
front and collects everything again. It has six tabs:

| Tab | Shows | From |
|---|---|---|
| Overview | OpenUSD's version, whether it has Python, where its libraries are, the executable, how many plugins are loaded; worker threads; the asset resolver, the other resolvers, URI schemes, search paths, the number of layers in memory; shading systems, source types, the number of shader nodes, shader and MaterialX search paths; how many TF_DEBUG flags are on | `pxr.h`, `PlugRegistry`, `WorkGetConcurrencyLimit`, `ArGetUnderlyingResolver`, `ArGetAvailableResolvers`, `ArGetRegisteredURISchemes`, `SdfLayer::GetLoadedLayers`, `SdrRegistry`, `UsdMtlxSearchPaths` |
| Plugins | Every USD plugin: its name, its type (library, python, resource), whether it's loaded, its path. Filterable | `PlugRegistry::GetAllPlugins` |
| File Formats | Each format's ID, extensions, target, whether it reads and writes, and its plugin. Filterable | the plugins' `plugInfo.json` metadata for `SdfFileFormat`'s subclasses |
| Render Delegates | Each one's name, ID, priority and plugin | `HdRendererPluginRegistry::GetPluginDescs` |
| Environment | The process's variables that start with `PXR_`, `TF_`, `HD`, `HGI`, `USD`, `SDF_`, `MATERIALX`, `OCIO` and so on (`isUsdEnvironmentVariable()`), and where each came from: the environment or Settings > USD. A button opens Settings > USD. Filterable | the process environment (on Windows, `GetEnvironmentStringsW`) |
| Debug Flags | Every `TfDebug` flag, with its description and a checkbox. A change applies at once, and the output goes to the log (docs/LOGGING.md §6). Changes aren't saved. Filterable | `TfDebug` |

Collecting loads no library. File formats and render delegates come from the plugins'
metadata, so opening the dialog doesn't load a format's or a render delegate's library.
Getting the shader registry runs its discovery, which the Material Editor does anyway.

**Refresh** collects everything again. **Copy** puts all of it on the clipboard as text,
with a heading per section, for a bug report (`usdRuntimeInfoText()`).

The data comes from `usdcc::usd::collectUsdRuntimeInfo()` (`usdcc_usd`, Qt-free). Tests:
`usd_runtime_info`, `usd_debug_flags` (`usdcc_tests`) and `usd_runtime_dialog`
(`usdcc_ui_tests`).

## 3. Settings > USD

OpenUSD reads some settings only from environment variables, and only once. Setting them
from the app works only before OpenUSD reads them. So `usdcc::usd::applyUsdEnvironmentSettings()`
runs in `main()` right after the preferences load (`installAppSettings()`), before
anything uses OpenUSD's plugin registry, asset resolver or shader registry. Every one of
these settings is `appliesAtNextStart`.

| Setting | Key | Becomes |
|---|---|---|
| USD plugin folders | `USD/pluginFolders` | `PXR_PLUGINPATH_NAME` |
| Asset search paths | `USD/assetSearchPaths` | `PXR_AR_DEFAULT_SEARCH_PATH` |
| MaterialX node folders | `USD/materialXFolders` | `PXR_MTLX_PLUGIN_SEARCH_PATHS` |
| Thread limit | `USD/threadLimit` | `WorkSetConcurrencyLimitArgument()` |

**The environment usdcc starts with stays in control**, so a studio's launcher can still
decide:
- A folder list adds the setting's folders after the variable's own and leaves out ones
  already there (`mergeSearchPaths()`).
- The thread limit is ignored when `PXR_WORK_THREAD_LIMIT` is set.

Variables are set with `_putenv_s` (`setenv` elsewhere), so the embedded Python's
`os.environ` and child processes see them too. Each one set is logged, and the
Environment tab says it came from Settings.

**The thread limit is a call, not the variable.** OpenUSD reads `PXR_WORK_THREAD_LIMIT`
while its libraries load, which is before `main()`. The TBB this OpenUSD build uses (from
before oneTBB) only takes a limit before anything has used TBB on the main thread. That's
another reason it's applied so early. If it didn't take, the log says so with a warning.

**Left out on purpose:**
- `TF_DEBUG`: it's read while the libraries load. The Debug Flags tab replaces it.
- `OCIO`: usdcc doesn't turn on Hydra's OCIO colour correction, so it would change nothing.
- `PXR_MTLX_STDLIB_SEARCH_PATHS`: it moves MaterialX's own libraries, which only a broken
  install needs.
- The roughly 90 tuning switches (`HDST_*`, `HGIGL_*`, `USDIMAGING_*`, ...): they belong in
  a launcher. The Environment tab shows any that are set.

A free-form list of any variable was considered and rejected. Some variables are read
before `main()`, so a list like that would fail silently for them.
