# Shared Python Packages

All five phases are built. Facts marked *tested* were run on 2026-11-05, on Windows 11. §7 says what wasn't.

A plugin says what it needs in `python.requires`, and the app installs that into one folder shared by all Python code: plugins and your own scripts.

```jsonc
// usdcc-plugin.json
"python": { "module": "notes", "requires": ["numpy>=2", "pyyaml"] }
```

What you see:

1. A plugin needs something that isn't there. It shows **Needs packages** in View > Plugins, with none of its code run, and a toast offers **Install**.
2. You get a list of what would change: packages to install, update and remove, at which versions, and for which plugins. You confirm. If nothing would change, it says everything is up to date.
3. It downloads in the background. You can close the window, and a toast says when it's done.
4. It takes effect at the next start, as turning a plugin on or off does.

**Update Packages** in the same panel resolves everything again, moving packages to their newest versions, and retries what couldn't be installed before.

## 1. The settings

All of these are on the Python page of Edit > Settings, and work from `usdcc.settings` too.

| Group | Label | Key | Type | Default | Meaning |
|---|---|---|---|---|---|
| Interpreter | Use my Python user packages | `Python/userSite` | Bool | off | `config.user_site_directory`: whether `%APPDATA%\Python\Python312\site-packages` is on `sys.path` |
| Packages | Shared packages folder | `Python/packagesFolder` | Folder | `%LOCALAPPDATA%\usdcc\python-env` | Where environments and uv's cache live |
| Packages | Additional package folders | `Python/extraPackageFolders` | String | empty | Ready-made folders, separated by `;`, ahead of the shared folder. The settings form of `USDCC_PYTHON_PATH` |
| Packages | My packages | `Python/userPackages` | String | empty | For your own scripts, separated by spaces: `numpy>=2 pyyaml` |
| Sources | Package index | `Python/packageIndex` | String | empty (PyPI) | uv's `--default-index` |
| Sources | Wheels folder | `Python/wheelsFolder` | Folder | empty | uv's `--find-links` |
| Sources | Don't use the network | `Python/offline` | Bool | off | Only the wheels folder and the cache: `--offline` |

The first three take effect at the next start, and the dialog says so while one differs from what the session started with. The others are read when an install runs, though what they install also waits for the next start.

**How they interact:**

- **User site on:** Python puts that folder ahead of the shared one, so a package there beats the installed copy. uv doesn't look there, so it still installs what plugins ask for.
- **Moving the shared folder:** nothing is moved or deleted. The new one starts empty, plugins show **Needs packages** at the next start, and the old folder stays until you delete it. uv's cache
  lives in the folder, so the new one downloads everything again.
- **`USDCC_PYTHON_PATH`** still works, ahead of Additional package folders. It's for launch scripts and has no row, so View > Plugins lists the folders in use.
- **My packages** is one line, so markers such as `; sys_platform == "win32"` aren't available there. Plugins keep the full syntax.

**Deliberately not settings:**

- **`PYTHONPATH`:** Python's own, and setting it is already a deliberate act.
- **Wheels only, and the block list** (§4): these protect the app. A setting would only be a way to crash it.
- **Where uv and `python.exe` are:** part of the deploy.

## 2. For studios and farm machines

```bash
usdcc --install-plugin-packages
```

This installs what the enabled plugins and My packages ask for, with nobody there to click, and exits before opening a window: 0 when everything went in, 2 when some was left out, 1 when it failed.
What happens goes to stderr and the log.

A studio can also point **Package index** at its own index, or set **Wheels folder** and **Don't use the network** for an air-gapped machine. A ready-made folder filled once for everyone belongs in
**Additional package folders**, as what it provides isn't installed again.

## 3. The folder

```text
%LOCALAPPDATA%\usdcc\python-env\
  current-cp312.txt       names the environment this Python uses
  cp312-1a2b3c4d5e6f\     an environment: packages, requirements.lock, environment.json, .in-use
  cp312-9f8e7d6c5b4a\     the next one, until nothing uses the old
  .cache\                 uv's cache
```

**It's a plain folder, not a venv.** A venv is a `site-packages` folder, plus a `python.exe` that makes that folder active when it starts. usdcc is the interpreter, so that start never happens and
the app would put the folder on `sys.path` itself. A `pyvenv.cfg` also holds an absolute path to the Python it came from (*tested*), which breaks when the app moves or updates. `uv pip sync --target` gives
what a venv would: one folder with exactly the listed packages.

**Every change makes a new folder, and never edits one in place.** Windows doesn't protect a loaded package: a loaded `.pyd` can simply be deleted. So changing a folder in place "works" and leaves the
running app wrong with no error. *Tested:* after a `sync` under a running process, `import yaml` failed, and `importlib.metadata` said numpy 2.3.0 while 2.5.4 was running.

So the steps are: resolve, install into `<name>.partial`, rename it, then replace the pointer last and atomically. A failed or cancelled install leaves things as they were (*tested*). It isn't wasteful, as files
are hardlinked from uv's cache and a new environment took 0.3 s (*tested*). That's why the cache lives here, on the same drive.

**Old folders go at startup, only if unused.** Since DLLs won't stop a delete, each running app holds an open `.in-use` file in its folder. Removing that file fails while it's held (*tested*), so a failed
removal means "leave this folder". Two apps installing at once are kept apart with a `QLockFile`, and the second is told.

**Python's version is part of the name.** Each Python has its own pointer, so another version's environment is ignored, as `cp312` `.pyd` files can't load in 3.13.

**`environment.json`** records My packages and what couldn't be installed, so that the app doesn't keep offering what it already knows can't work, until the plugin asks for something else.

## 4. How installing works

**uv, bundled and pinned, like ruff** (`cmake/modules/UsdccUv.cmake`, deploy `tools\uv\`). It adds 31 MB. pip would also need pip in the deploy, and has no equivalent of `sync --target` to remove what's no longer listed.

**uv needs a Python, so the deploy has one.** Even resolving fails without an interpreter (*tested*). `python\python.exe` is 105 KB and reuses the deploy's `Lib` and `DLLs`. It finds `python312.dll` in `bin\`,
which the app puts on `PATH` when it runs uv. `tools/deploy.py` checks that the two work together.

**Only requirements reach uv.** `python.requires` is the one place, with no separate `requirements.txt`. Only names, versions and markers are allowed: **a URL refuses the plugin,** and `_plugins.py` validates
each line with `packaging` first, so a manifest can't slip in `--index-url` through a requirement. Also taken off the list is what the app provides, what a plugin's own `deps/` meets, and what
ready-made folders meet.

**Extra indexes, named in the open: `python.indexes`.** A plugin may need wheels
from a studio's own index rather than PyPI. A manifest may name that index:

```json
"python": { "module": "studio_render", "requires": ["studio-sdk>=1,<2"], "indexes": ["https://packages.example.com/simple"] }
```

- **Only plain https URLs,** with no user name, password, query or fragment, and only with `requires`. Anything else refuses the plugin (`packageIndexProblem()` in plugin_manager.cpp).
- **They go to uv as `--index`, ahead of the default index.** uv takes a package from the first index that has it, so the plugin's packages come from its index, and what that index doesn't have (numpy, for NVIDIA's) still comes from PyPI.
- **Only for the plugin that named them:** they're passed while resolving that plugin and those resolved with it, and while installing the result. Another plugin can't get a package from them on its own (*tested*).
- **The user sees them before anything is downloaded from them.** The install dialog lists "From other package indexes", each with the plugins that named it, and the log and `--install-plugin-packages` say "<plugin> also uses the package index <url>". Resolving does read the index's listing first, to work out the plan.
- **Wheels only, still.** An index changes where wheels come from, not that nothing runs during an install.

This relaxes the old rule that a manifest can't pick where packages come from. The trade-off: a plugin already runs with full rights once installed, so the index adds little, and naming it in the manifest, shown before installing, beats sending users to the global **Package index** setting. That setting would move every package to NVIDIA's index, where numpy isn't.

**Installing is always asked for, never at startup.** Startup uses no network and only checks. **Wheels only** (`--only-binary :all:`), so nothing runs during an install, as unpacking a wheel executes nothing. The
price is that a source-only package can't be installed, and the error says so (*tested*). Importing a package still runs its code, but the plugin asking for it already runs with full rights.
`--no-config` stops a stray `uv.toml` on the machine from changing what's installed, and `--system-certs` is passed for corporate proxies.

**One plugin at a time, in activation order,** because uv finds a conflict but doesn't say whose it is: "you require numpy>=2 and numpy<2" (*tested*). A plugin that doesn't fit is left out, and the rest still install.
To name the other plugin, the refused one is tried beside each accepted one, only when it failed. It costs little: 0.03 s per step when cached (*tested*). uv's exit code separates "these can't work together" (1) from
"couldn't try", such as no network (2), so a dropped connection isn't blamed on a plugin (*tested*).

**Adding a plugin doesn't change versions others use.** The previous lock holds them (*tested*). **Update Packages** passes `--upgrade` (*tested*).

**Packages that would break the app are never installed:** PySide6, `usd-core`, PyQt and the like (`_NEVER_INSTALLED`), through a constraints file of `pyside6<0` and so on, which stops indirect ones too (*tested*). A
plugin naming what the app provides is fine, as today. A package that truly needs PySide6 from pip is refused, which is safe, but stricter than it could be.

**`check_modules()` stays the gate at every start,** treating what's in the folder as installed. It reports whether installing could fix a refusal, which is what **Needs packages** means: something is missing, or an
installed copy in the shared folder doesn't suit, and a new resolve could move it. An installed copy in a ready-made folder is fixed, so that plugin is plain refused.

**Whose packages:** My packages first, then every enabled plugin. A turned-off plugin doesn't take part, so it can't block others. Its packages go at the next install, and until then they do nothing, being only importable.

**`deps/` still works,** and suits a plugin that must carry everything (PLUGINS.md §3.10). The docs just stop recommending it.

**Left out:**

- **A process per plugin,** for versions that truly can't coexist. As in PLUGINS.md §3.10, that's up to its author.
- **Activating without a restart.** Panels have to exist before the layout is restored (PLUGINS.md §3.3).

## 5. Two bugs this fixed

**The app read the user's own Python packages.** `PyConfig_InitPythonConfig` leaves the user site on, so the deployed app had `%APPDATA%\Python\Python312\site-packages` on `sys.path`, ahead of any shared folder. A plugin could run
here and fail elsewhere, using something it never asked for, and a package in that folder would beat the one installed for the plugin. It's now the setting above, off by default. Anything you were importing from there stops working in
the app until you tick it.

**`check_modules()` could refuse the wrong plugin.** When an installed copy couldn't satisfy everyone, it refused whichever plugin needed it last, not the one it didn't suit. With numpy 2.5.4 installed, `numpy>=2` ran alone, but beside a plugin
needing `numpy<2` both were refused, the good one with "It needs numpy>=2, but the installed numpy is 2.5.4".

Fixed along the way: a marker on a manifest's requirement wasn't honoured, so `foo; sys_platform == "linux"` was required on Windows. And a plugin is now told everything it's missing at once, not one package per start.

## 6. Where the code is

- **`src/python/usdcc/_plugins.py`:** `check_modules()`, `package_requests()`, `_NEVER_INSTALLED`.
- **`scripting/package_environment.h`:** `PackageEnvironment` (the folder) and `PackageInstaller` (running uv through `QProcess`, which keeps the app responsive and doesn't hold Python's lock).
- **`scripting/plugin_manager.h`:** `NeedsPackages`, `packageRequests()`, `installerOptions()`, `packagesPending()`, `installPackages()`.
- **`scripting/package_install_dialog.h`, `plugin_manager_side_panel.cpp`:** the UI.
- **`scripting/python_settings.h`:** the page.
- **`core/settings.h`, `ui/settings_dialog.cpp`:** the `Folder` type and `appliesAtNextStart`.
- **`cmake/modules/UsdccUv.cmake`, `tools/deploy.py`:** uv and `python.exe`.
- **`tests/package_environment_test.cpp`:** the real uv, with wheels the tests make, and no network.

## 7. Tested, and not

**Tested in a deployed app, with real downloads from PyPI, driven by scripts clicking the real widgets:**

- **The whole flow:** three plugins needing packages show **Needs packages**. **Install Packages** lists versions and names the conflicting plugin, and installing puts an environment in place. At the next start the plugins that fit run from it, with PySide6 and `pxr` still coming from the app, the one that doesn't is plain refused with why, and the button is gone.
- **The startup toast:** it's up when packages are needed, and its **Install...** opens the dialog. A grab of the window doesn't show toasts, so this was checked by their state and by clicking, not by eye.
- **Closing the window during an install:** it carries on, and a toast appears at the end.
- **`--install-plugin-packages`:** installs with no window, exits 2 with one plugin left out, and reports on stderr.
- **The user site setting, off by default:** nothing from it is on `sys.path`, and a package only found there is reported missing.
- **scipy 1.18 and opencv-python 5.0 in the app,** from a folder uv filled, computing, with USD and the window fine afterwards. That's 268 MB with numpy. opencv's Windows wheel has no Qt files.

**Tested with the real uv and wheels the tests make:** conflicts, indirect blocked packages, kept versions, updates, new folders, `.in-use` protection, no network, cancelling between resolving and installing, two installs at once, no uv, and `--offline` with `--find-links` together. A plugin's own index (`package_extra_index`), as a local simple index: its package comes from there for the plugin that named it, and the plugin that didn't can't get it. Manifest indexes that are refused: http, file, with a password, and without `requires` (`plugin_package_indexes`).

The retired Python RTX plugin previously exercised the NVIDIA index in a local
scratch-folder test. Native RTX now loads SDK DLLs directly and declares no Python
requirements or indexes; its old user copies are excluded from package planning.
Generic extra-index support and its tests remain for other plugins.

**Not tested:**

- Corporate proxies and `--system-certs`, and a real package index other than PyPI.
- Whether the block list refuses a package for the wrong reason in practice. No real package tried needs PySide6.
- Older Windows versions and network drives. Deleting a loaded `.pyd` was seen on this Windows 11 build only. The design doesn't rely on it either way, as the `.in-use` file decides.
- A mixed import: a package half-replaced under a running app. Missing modules and wrong metadata were seen, not a crash, and numpy's modules were already loaded in that test. New folders avoid the question.
- Packages with special needs, such as GPU builds of torch. They're large, and may bring DLLs that clash with USD's.
- What your own scripts import from the user site. The check above only started the app and read the plugin list.
- A shared folder on a network or read-only drive. The `.in-use` file can't be created on a read-only one, hardlinks don't cross drives, and loading `.pyd` files from a share may be slow. **Additional package folders** is the better fit for a ready-made studio folder.
- A lock left behind by a crash during an install. It relies on `QLockFile` noticing the process is gone.
- Cancelling while uv is running, by button. The tests cancel between steps. Clicking by hand wasn't done either: everything above went through scripts.
- Ticking **Use my Python user packages** and restarting. Only the default, off, was run. The same goes for **Additional package folders** and **Package index**, set through the dialog.
