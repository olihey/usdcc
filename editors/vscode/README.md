# usdcc for VS Code

Run Python scripts and reload plugins in a running [usdcc](https://github.com/olihey/usdcc), from VS Code.

usdcc lets editors connect unless Settings > Python > **Allow editors to connect** is off. The status bar
shows the app and its current stage; with several running, click it to pick one.

| Command | Key | |
|---|---|---|
| usdcc: Run File | `Ctrl+Enter` | Runs the editor's text, saved or not, as one undo step. In a plugin's folder it reloads the plugin |
| usdcc: Run Selection | `Ctrl+Shift+Enter` | The selected lines, or the cursor's line |
| usdcc: Reload Plugin | | Saves the plugin's files and reloads it |
| usdcc: Attach Debugger | | Breakpoints in scripts and plugins, with the Python Debugger extension |
| usdcc: Start usdcc | | Starts it with the workspace's plugins, in a profile of its own |
| usdcc: Set Up Workspace | | Completion of `usdcc`, `pxr` and PySide6 in this workspace, with the Python extension |
| usdcc: Stop Script | | Stops what usdcc is running |
| usdcc: New Plugin... | | From a template, running at once |
| usdcc: Connect | | Picks the app |
| usdcc: Keep Variables Between Runs, Reset Variables | | |

Output and tracebacks are in the **usdcc** output channel, where a traceback's places are links.
`usdcc.reloadPluginOnSave` reloads a plugin whenever one of its files is saved.

## Debugging

F5, or a `launch.json` entry, starts usdcc and attaches; with `"request": "attach"` it attaches to the one
connected to. While a breakpoint is hit, usdcc's window is frozen, as scripts run on its main thread. Stopping
the debugger leaves usdcc running.

```json
{ "type": "usdcc", "request": "launch", "name": "Start usdcc", "stage": "${workspaceFolder}/shot.usda" }
```

## Plugins

The usdcc icon in the activity bar lists the running app's plugins, with why one isn't running. A plugin that failed, and a
script that did, are in Problems. `usdcc-plugin.json` is completed and checked as you type, and snippets start with `usdcc-`.
