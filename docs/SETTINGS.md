# Settings

usdcc's preferences: one registry of typed, described settings that the app's C++ code,
Python scripts and the Settings dialog (Edit > Settings…, `Ctrl+,`) all read and write.
Part of M10 (docs/PLAN.md §7).

## 1. Preferences and UI state

`usdcc.ini` (in the app config folder, `%LOCALAPPDATA%\usdcc` on Windows) holds two kinds
of values:

- **Preferences** are choices the user makes on purpose: the accent color, the UI
  scale, which key picks the Move tool. They live in the registry (§2) and appear in the
  dialog.
- **UI state** is what the app remembers about how it was left: the dock layout, window
  geometry, splitter positions, a panel's filters, dialog sizes. Panels keep writing it
  directly with `usdcc::ui::appSettings()`, and it doesn't belong in the dialog.

Both kinds sit in the same file. A preference's key is its INI `group/name`.

## 2. The registry: `usdcc::core::Settings`

`usdcc/core/settings.h`, in `usdcc_core`. It is Qt-free, like the rest of that library,
and lives in its shared library, so the exe and the Python modules see one instance:
`Settings::instance()`.

**Definitions.** A setting is defined once with a `SettingDefinition`:

| Field | Meaning |
|---|---|
| `key` | `"Group/name"`: also where it is saved (`Appearance/accentColor`, `Shortcuts/tools.move`) |
| `category` | Its page in the dialog: `Appearance`, `Viewport`, `Tools`, `Shortcuts`, `Python`, or a plugin's own |
| `group` | A heading on that page (`Theme`, `Interface`, `Tools`) |
| `label`, `description` | What the dialog shows |
| `type` | `Bool`, `Int`, `Float`, `String`, `Choice`, `Color` (`"#rrggbb"`), `Shortcut` (`QKeySequence` portable text such as `"W"` or `"Ctrl+Shift+S"`; `""` for none) |
| `defaultValue` | A `SettingValue`: `std::variant<bool, int, double, std::string>` |
| `minimum`, `maximum` | An Int or Float range, used when `maximum > minimum` |
| `choices` | A Choice setting's values |

**Values.**
- `define()` loads the saved value. If none is saved, or the saved value no longer fits
  the definition, the setting gets its default.
- `setValue()` checks the type, range, choices and color format. It writes through to the
  backend and fires `changed(key)`. An unchanged value fires nothing.
- `reset()` restores the default and removes the key from the file, so a later change of
  the default in code applies.
- Defining a key again replaces its description and keeps its value.
- `defined(key)` fires for every definition, which is how the dialog picks up a plugin's
  settings while it is open.

**Storage.**
- Where values are stored is a `SettingsBackend`.
- `main.cpp` calls `usdcc::ui::installAppSettings()` before the theme is applied. That
  installs a `QSettingsBackend` on `usdcc.ini`, so every setting defined after that starts
  from its saved value.
- Until then, and in tests, a `MemorySettingsBackend` holds values in memory.
  `tests/test_settings.h` installs a fresh one for each UI test that needs it.

The registry is not thread-safe: use it from the UI thread.

## 3. Using a setting in C++

```cpp
// Once, where the feature lives (theme.cpp, viewport_tool_bars.cpp, ...):
usdcc::core::Settings::instance().define({
    .key = "Viewport/showGrid", .category = "Viewport", .group = "Display",
    .label = "Show grid", .type = usdcc::core::SettingType::Bool, .defaultValue = true});

// Read it where it's needed, and follow changes:
bool grid = usdcc::core::Settings::instance().get<bool>("Viewport/showGrid");
usdcc::ui::onSettingChanged(this, "Viewport/showGrid", [this]() { update(); });
```

`usdcc/ui/app_settings.h` adds the Qt glue:

- `settingColor()` and `settingShortcut()` return a value as a `QColor` or `QKeySequence`.
- `onSettingChanged()` and `onSettingsChanged()` call back for as long as a `QObject`
  lives.
- `bindShortcut(action, key)` keeps a `QAction`'s shortcut equal to a Shortcut setting.

Pass string defaults as `std::string("...")`, not as a bare literal, so the variant can't
take them for a `bool`.

## 4. The settings so far

| Key | Type | Default | Where |
|---|---|---|---|
| `Appearance/accentColor` | Color | `#3d8bfd` | `theme.cpp` |
| `Appearance/uiScale` | Int 80–200 | 100 | `UiScaleController` (also View > UI Scale, Ctrl+Numpad+/-) |
| `Shortcuts/tools.select` | Shortcut | `Q` | `ViewportToolBars` |
| `Shortcuts/tools.move` | Shortcut | `W` | `ViewportToolBars` |
| `Shortcuts/tools.rotate` | Shortcut | `E` | `ViewportToolBars` |
| `Shortcuts/tools.scale` | Shortcut | `R` | `ViewportToolBars` |

**Accent color.** `dark.qss` and `dock.qss` use three tokens:

| Token | Replaced with |
|---|---|
| `@accent` | the accent color |
| `@accent-rgb` | its `r, g, b`, as in `rgba(@accent-rgb, 25%)` |
| `@accent-hover` | a lighter shade |

- `theme::resolveTokens()` replaces them before UI scaling, and the theme is re-applied
  whenever the setting changes.
- C++ painting reads `theme::accent()` at paint time: the palette, text-view selections,
  the Outliner's ancestor tint and reference badges, the Layers panel's edit target, and
  number-field sliders.
- The Z axis color, the syntax-highlighting colors, the viewport's orange selection and
  the app icon stay as they are.

**Tool shortcuts.**
- They are application-wide shortcuts on the tool rail's actions, so they also work in
  floating panels and with the focus in the viewport's native window.
- Text fields keep their letters: they claim printable keys first.
- Item views such as the Outliner lose type-to-search for those keys.
- The rail's tooltips show the current key.

## 5. The dialog

`usdcc::ui::SettingsDialog` (`settings_dialog.h`) opens from Edit > Settings…, one instance
at a time.

**Layout.**
- Categories are listed on the left, those the app knows first (Appearance, Viewport,
  Tools, Shortcuts, Python), then the others in definition order.
- Every page is generated from the definitions: a heading per group and a row per
  setting. A row holds the label, an editor for the type, the description, and a Reset
  button that shows while the value isn't the default.
- Categories without settings get no page.

**Editors.**

| Type | Editor |
|---|---|
| Bool | check box |
| Int / Float | spin box |
| Choice | combo box |
| String | line edit |
| Color | swatch button that opens Qt's color dialog |
| Shortcut | one-chord key sequence editor with a clear button |

- The color follows the picker while it is open, at most every 100 ms. Cancel restores
  the old color.
- A Shortcut row flags a key that another shortcut already uses.

**Behavior.**
- Changes apply and save immediately. There is no OK or Cancel.
- Enter only commits the field it's typed in. It never closes the dialog, because there
  is no default button. Escape and Close do close it.
- Editors follow changes made elsewhere: the View menu, or Python.
- *Reset Page to Defaults* resets the shown category.

## 6. Python: `usdcc.settings`

The functions are bound in `_usdcc_app` (`app_module.cpp`) and wrapped by
`src/python/usdcc/settings.py`:

```python
import usdcc.settings as settings
settings.get("Appearance/accentColor")             # '#3d8bfd'
settings.set("Appearance/accentColor", "#e5484d")  # applies and saves at once
settings.reset("Appearance/accentColor")
settings.keys("Shortcuts"), settings.categories(), settings.info("Shortcuts/tools.move")

settings.define("MyPlugin/autoSave", "bool", True, label="Save automatically",
                category="My Plugin", description="Save after every edit.")
```

- `set()` raises `ValueError` for a value that doesn't fit, and `KeyError` for an unknown
  key.
- `define()` takes the type as a string: `bool`, `int`, `float`, `string`, `choice`,
  `color` or `shortcut`. It also takes `minimum`/`maximum` (give both) and `choices`.
- A defined setting gets a row in the dialog, on a page of its own category.
- Not yet: change callbacks into Python. Poll with `get()` where it matters.

## 7. Tests

| Suite | File | Covers |
|---|---|---|
| `usdcc_tests` | `settings_test.cpp` | values, type and range checks, definitions, backends |
| `usdcc_ui_tests` | `settings_ui_test.cpp` | the INI round trip, the accent reaching stylesheets and palette, the dialog's pages and editors, the tool shortcuts with real key presses (and a text field keeping its letter) |
| `usdcc_python_tests` | `python_app_test.cpp`, `python_settings` | the Python module |
