# usdcc package (docs/SCRIPTING.md section 4).
#
# Pure-Python modules (usd, app, core, tools, log, ui) sit next to the compiled
# ones they use: inside the app, the embedded modules _usdcc_app and
# _usdcc_log; in a plain python, _usd.pyd; for usdcc.ui, _ui.pyd, which needs
# PySide6. Nothing is imported here, so `import usdcc.log` (which the app does
# at start-up) doesn't load PySide6.
