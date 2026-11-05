"""usdcc.settings: the app's preferences (docs/SETTINGS.md).

The same settings the Settings dialog (Edit > Settings...) shows; a change
applies immediately, saves to usdcc.ini and updates the dialog if it is open.

    import usdcc.settings as settings
    settings.get("Appearance/accentColor")             # '#3d8bfd'
    settings.set("Appearance/accentColor", "#e5484d")  # the UI turns red
    settings.set("Shortcuts/tools.move", "T")          # T picks the Move tool
    settings.reset("Shortcuts/tools.move")             # back to W

A plugin can add its own settings; they get a page in the dialog:

    settings.define("MyPlugin/autoSave", "bool", True,
                    label="Save automatically", category="My Plugin")
    if settings.get("MyPlugin/autoSave"): ...

Types: "bool", "int", "float", "string", "choice" (one of `choices`),
"color" ("#rrggbb"), "shortcut" (key text such as "W" or "Ctrl+Shift+S",
"" for none) and "folder" (a path, "" for none, with a Browse button).

Needs the running app.
"""

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None


def _app():
    if _native is None:
        raise RuntimeError("usdcc.settings needs the running usdcc app")
    return _native


def get(key):
    """The current value of setting `key`. KeyError for an unknown key."""
    return _app().settings_get(key)


def set(key, value):  # noqa: A001 - the module's verb, as in settings.set()
    """Changes setting `key`; applies and saves at once.

    ValueError if `value` doesn't fit the setting (its type, range or choices),
    KeyError for an unknown key.
    """
    _app().settings_set(key, value)


def reset(key):
    """Puts setting `key` back to its default."""
    _app().settings_reset(key)


def is_default(key):
    """Whether setting `key` has its default value."""
    return _app().settings_is_default(key)


def keys(category=""):
    """The settings' keys, in definition order; only `category`'s if given."""
    return _app().settings_keys(category)


def categories():
    """The categories (the Settings dialog's pages) that have settings."""
    return _app().settings_categories()


def show(category=""):
    """Opens the Settings dialog (or brings it to the front), on `category`'s page
    if given. Needs the main window."""
    _app().settings_show(category)


def info(key):
    """A dict describing setting `key`: key, type, category, group, label,
    description, default, value and applies_at_next_start, plus minimum/maximum
    and choices where the setting has them."""
    return _app().settings_info(key)


def define(key, type, default, label="", category="Python", group="", description="",  # noqa: A002
           minimum=None, maximum=None, choices=None, applies_at_next_start=False):
    """Adds a setting (or updates its description if `key` exists, keeping
    its value). Its saved value, if any, is loaded at once.

    `key` is "Group/name" and is where the value is saved in usdcc.ini; give
    the setting its own group, such as your plugin's name. `category` is the
    page of the Settings dialog it appears on, `group` a heading on that page.
    `minimum`/`maximum` bound an int or float (give both); `choices` lists a
    choice setting's values. `applies_at_next_start` makes the dialog say a
    change takes effect at the next start. ValueError if the definition is invalid.
    """
    _app().settings_define(key, type, default, label=label, category=category, group=group,
                           description=description, minimum=minimum, maximum=maximum,
                           choices=list(choices or []), applies_at_next_start=applies_at_next_start)
