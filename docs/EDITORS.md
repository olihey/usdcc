# Scripting usdcc from VS Code

Write Python scripts and plugins in VS Code, with completion, and run and debug them in the usdcc you have open.

## Set up, once

1. **In usdcc,** open the Script Editor's ☰ menu and pick **Set Up VS Code...**, with a script of yours open, or
   pick a folder. It says when it's done, and offers **Install Extension** and **Open in VS Code**.
2. **In VS Code,** install Microsoft's **Python** extension, if you don't have it. It brings completion and the debugger.

No usdcc open yet? In VS Code, **usdcc: Start usdcc** in the command palette (`Ctrl+Shift+P`) starts the installed one.

## Run a script

Open a Python file. The status bar shows the usdcc you're connected to, and its current stage.

| To | Do |
|---|---|
| Run the file, saved or not | `Ctrl+Enter` |
| Run the selected lines | `Ctrl+Shift+Enter` |
| Stop it | **Stop** in the status bar |
| Keep variables between runs | **usdcc: Keep Variables Between Runs** |

A run is one step in Edit > Undo. Its output is in VS Code's **usdcc** output, with links in tracebacks, and an error marks
its line. While a script runs, or usdcc shows a dialog, another is refused.

## Make a plugin

1. **usdcc: New Plugin...**, and pick what it starts with: a menu action, an entry in the Outliner's menu, a panel, a tool or a
   renderer. It's running at once. Look in the Tools menu, the Stages menu or the tool rail.
2. Change it, and press `Ctrl+Enter`: the plugin is saved and reloaded, with no restart. With the setting
   **usdcc.reloadPluginOnSave**, saving is enough.
3. The usdcc icon in VS Code's activity bar lists the plugins, and why one isn't running.

`usdcc-plugin.json` is completed and checked as you type. Snippets start with `usdcc-`. [PLUGINS.md](PLUGINS.md) says what a
plugin can do.

## Debug

- **F5** starts a usdcc with the plugins of your workspace and attaches the debugger. It has settings and layouts of its own, so a
  broken plugin can't disturb yours.
- **usdcc: Attach Debugger** attaches to the usdcc you're connected to.

Breakpoints stop in scripts and in a plugin's menu actions, panels and tools. **usdcc's window is frozen meanwhile,** as scripts run on
its main thread. Stopping the debugger leaves usdcc running.

## Who can connect

Only programs you run yourself, on this machine. Turn it off in Settings > Python > **Allow editors to connect**. A debugger can attach
only after you ask for it, and then until usdcc quits.

## If something is wrong

| You see | Why |
|---|---|
| `usdcc: not running` | No usdcc with **Allow editors to connect** is open. F5's has its own profile, and is found by itself |
| `usdcc: pick one` | Several are open: click it |
| `usdcc: update needed` | The extension and usdcc are of different ages: **Set Up VS Code...** installs usdcc's own |
| No completion of `usdcc` or `pxr` | **usdcc: Set Up Workspace**, in that folder |
| A run that never ends | usdcc may be showing a dialog the script opened |
| ".vscode/settings.json isn't plain JSON" | It has comments, which usdcc would lose: use **usdcc: Set Up Workspace** instead |
