# Themes

Settings > Appearance > Theme chooses the colors of the whole application. A change applies
at once. The choices:

- **System**: usdcc Dark or usdcc Light, as the system's own setting is, and it follows it.
- **usdcc Dark** (the default) and **usdcc Light**.
- **Neutral Grey**, **Nord**, **Catppuccin Mocha** and **Catppuccin Latte** (light). Nord and
  Catppuccin are based on the palettes of those names, with some colors changed for contrast.
- Your own (§2).

## 1. How it works

- A **token** (`usdcc::ui::theme::Token`) names what a color is for. The table in `theme.cpp`
  has every token's name and its color in usdcc Dark and usdcc Light.
- `resources/theme/app.qss` and `dock.qss` name tokens: `@surface-panel` is a token's color, and
  `rgba(@ink-rgb, 50%)` the same at an opacity.
- C++ painting calls `theme::color(Token::SurfacePanel)` when it draws. What has to keep a color
  (a tree item's, a text format's) sets it again in `theme::onChanged()`.
- `theme::icon(":/usdcc/theme/x.png")` draws the line icons in the theme's colors. The images the
  stylesheets name are recolored into a temporary folder, reached as `usdcc-tinted:`.
- **The accent** (`Appearance/accentColor`) defaults to the theme's. One chosen in Settings stays
  with every theme, and the text on it is dark when it is bright.
- **Ink** (text at an opacity) is made stronger in light themes, where the same opacity reads weaker.
- **Fixed in every theme**: the axes' colors, a shader node's header and ports, the render view's
  background, the HUD's plates. The viewport's background is its own setting
  (`Viewport/backgroundColor`) and doesn't follow the theme. The web pages (`usdcc.css`) stay dark.

## 2. Your own theme

A JSON file in the `themes` folder of usdcc's data folder (Windows:
`%LOCALAPPDATA%\usdcc_config\themes`), read when usdcc starts:

```json
{
    "name": "My Theme",
    "base": "dark",
    "colors": {
        "surface-panel": "#202a30",
        "accent": "#c08040"
    }
}
```

- `name`: what Settings lists. It can't be one that's there already.
- `base`: `dark` (the default) or `light`. Colors that aren't named are usdcc Dark's or usdcc Light's.
- `colors`: tokens, as `#rrggbb`. A file with an unknown token or an invalid color isn't used, and
  the Log panel says why.

| Token | usdcc Dark | usdcc Light |
|---|---|---|
| `surface-bar` | `#161618` | `#e4e4e8` |
| `surface-toolbar` | `#1a1a1c` | `#ebebee` |
| `surface-panel` | `#1c1c1e` | `#f3f3f5` |
| `surface-raised` | `#232326` | `#fcfcfd` |
| `surface-hover` | `#2c2c30` | `#e6e6ea` |
| `surface-readonly` | `#222225` | `#ececef` |
| `inset` | `#141416` | `#ffffff` |
| `gap` | `#0e0e10` | `#c9c9ce` |
| `bevel-light` | `#2a2a2e` | `#ffffff` |
| `bevel-midlight` | `#242428` | `#ebebee` |
| `bevel-mid` | `#333337` | `#c8c8cd` |
| `shadow` | `#000000` | `#c8c8cd` |
| `text` | `#e8e8ea` | `#1c1c1f` |
| `text-strong` | `#ffffff` | `#000000` |
| `ink` | `#ffffff` | `#000000` |
| `overlay` | `#ffffff` | `#000000` |
| `muted` | `#8a8d93` | `#63666d` |
| `muted-strong` | `#a8aab0` | `#4f5258` |
| `icon` | `#a0a0a4` | `#5c5c62` |
| `icon-disabled` | `#4a4a4e` | `#b8b8be` |
| `icon-grip` | `#68686e` | `#9a9aa0` |
| `accent` | `#3d8bfd` | `#2f6fe0` |
| `accent-text` | `#3d8bfd` | `#1f5fcf` |
| `on-accent` | `#ffffff` | `#ffffff` |
| `error` | `#ff5f56` | `#c9302c` |
| `warning` | `#ffb340` | `#9a5b00` |
| `attention` | `#ffbd2e` | `#946000` |
| `success` | `#4fd671` | `#1a8a3c` |
| `syntax-keyword` | `#3d8bfd` | `#0b57d0` |
| `syntax-builtin` | `#8ab8ff` | `#3a68ad` |
| `syntax-string` | `#4fd671` | `#1a7f37` |
| `syntax-number` | `#ffbd2e` | `#985600` |
| `syntax-path` | `#e5c07b` | `#835500` |
| `syntax-key` | `#c8c8cc` | `#3a3a40` |
| `prim-instance` | `#87cefa` | `#0b6cab` |
| `prim-arcs` | `#de9e2e` | `#8f5a00` |
| `prim-prototype` | `#7688d9` | `#4456b4` |
| `graph-grid-fine` | `#242426` | `#e6e6ea` |
| `graph-grid-coarse` | `#2e2e31` | `#d6d6db` |
| `graph-connection` | `#828288` | `#85858b` |
| `node-body` | `#2c2c30` | `#fcfcfd` |
| `node-page` | `#36363a` | `#e8e8ec` |
| `node-border` | `#46464a` | `#c0c0c6` |
| `node-border-hover` | `#6e6e74` | `#909096` |
| `node-text` | `#e1e1e4` | `#202024` |
| `node-text-muted` | `#96969b` | `#6a6a70` |
| `node-page-text` | `#cdcdd2` | `#44444a` |
