# Arnold pack in CI

Building the Arnold pack (hdArnold and its three sibling plugins, `deps/usd-plugins/arnold.json`)
in CI, without the Arnold SDK ever reaching the public repository. A plan: nothing here is set up
yet.

**Status (2026-11-26):** the pack is `local`. It builds by hand on Windows (7.5.4.0) and on macOS
arm64 (7.5.4.2) and renders in usdcc's viewport on both ([USD_PLUGINS.md](USD_PLUGINS.md) §3,
Arnold). `deps-usd-plugins.yml` leaves the `local` set out entirely.

## 1. Decisions

- **The Arnold core is never shipped.** Not `ai.dll`, `libai.dylib`, nor any other SDK file, in
  any pack, artifact or release. The user names their own Arnold in Settings > Renderers >
  Arnold, as now. §6 says why.
- **The build runs in a private repository of its own**, which also holds the SDK. The public
  repository gets no secret and no step that touches the SDK. Rejected: keeping only the SDK
  private and building in `olihey/usdcc`. Its artifacts would be public (any signed-in GitHub
  user can download a public repository's artifacts), which publishes hdArnold before §6.3 is
  settled, and every edit of a public workflow holding a secret needs care with logs, caches
  and forks.
- **The private workflow builds with the public repository's own tools**, at a ref it is given:
  `tools/ci/usd_plugin_asset.py`, `tools/build_usd_plugin.py`, `tools/ci/check_usd_plugin.py`.
  The pin stays in `olihey/usdcc`, so a local build and CI's are the same build, as for every
  other pack.
- **Its packs are kept as release assets of the private repository**, under the same names
  `usd_plugin_asset.py` gives (`usdplugin-arnold-<commit>-<hash>-<platform>-py313.zip`) and in a
  release named as the OpenUSD asset's tag (`deps/usd-…`), so a pack is tied to the OpenUSD it was
  built against.
- **Platforms: `win64` and `macos-arm64`**, the pin's. Linux and Windows arm64 later (§7).

## 2. The private repository

Proposed name: `olihey/usdcc-arnold` (open, §5 item 1). Private; collaborators only people who
hold an Arnold licence of their own: access to the repository hands over the SDK, which the
Terms of Use forbid (§8.1, "make available … to a third party").

```text
usdcc-arnold/
  README.md                     what this is, who may have access, how to install a pack
  .github/workflows/arnold.yml  the build (§3)
  sdk.json                      which SDK archive each platform uses, with its SHA-256
```

The SDKs are release assets of a release `sdk` (not in Git: `ai.dll` is 174 MB and GitHub refuses
files over 100 MB; release assets allow 2 GiB each and have no bandwidth quota, which LFS has):

| Asset | From | Size |
|---|---|---|
| `Arnold-7.5.4.0-windows.zip` | Autodesk Account, the Windows SDK the pin was proven with | ~ (`ai.dll` alone 174 MB) |
| `Arnold-7.5.4.2-darwin.tgz` | Autodesk Account, `~/deps/Arnold-7.5.4.2-darwin` | 1.2 GB unpacked (`libai.dylib` 331 MB, universal) |

A trimmed SDK is enough and much smaller (§5 item 3): the build needs `include/`, the core
(`bin/ai.dll` with `lib/ai.lib`; `bin/libai.dylib`) and `EULA.txt`. The check also loads the core,
which loads its licensing libraries from beside it (macOS: `libAdskLicensingSDK.10.dylib`,
`libAdClmHub.3.dylib`, `libcer.dylib` in `bin/`), so those stay too.

## 3. The workflow

`arnold.yml`, started by hand (`workflow_dispatch`) with two inputs: the `olihey/usdcc` ref
(default `master`) and `replace` (build again over a pack already there). One job per platform,
`windows-2022` and `macos-15`, as `deps-usd-plugins.yml`'s, whose steps it copies:

1. **Check out** `olihey/usdcc` at the ref, into the workspace (public: no token).
2. **Name the pack**: `python tools/ci/usd_plugin_asset.py arnold --github-output` gives `name`,
   `zip`, `tag`, `usd_zip`, `python`.
3. **Already built?** The private release `tag` has `zip`: stop, unless `replace`.
4. **Set up Python** (`python`, 3.13.12), and on macOS `brew install ninja uv`; as
   `deps-usd-plugins.yml` does on each system.
5. **Fetch OpenUSD**: `gh release download <tag> --repo olihey/usdcc --pattern <usd_zip>`
   (public), unzipped into `$RUNNER_TEMP/deps/usd`.
6. **Fetch the SDK**: `gh release download sdk --pattern <its archive>` with the job's own
   `GITHUB_TOKEN` (the repository's own release, so no extra secret), its SHA-256 checked against
   `sdk.json`, unpacked into `$RUNNER_TEMP/arnold`. Never printed, cached or uploaded.
7. **Build**: `python tools/build_usd_plugin.py arnold --usd-install … --work … --pack-dir …
   --sdk arnold=$RUNNER_TEMP/arnold`.
8. **Zip**, as `deps-usd-plugins.yml` (`zip -r -y` on macOS, `7z` on Windows).
9. **No SDK file in it**: fail if the zip holds any file whose name is in the SDK's `bin/` or
   `lib/` (`ai.dll`, `libai.dylib`, `ai.lib`, the licensing libraries). The pack is 13 files today.
10. **Check it over OpenUSD**: unzipped elsewhere, build folders deleted,
    `python tools/ci/check_usd_plugin.py arnold --usd … --pack-dir … --sdk arnold=$RUNNER_TEMP/arnold
    --forbid … --forbid …`. It preloads the core and checks the four plugins and the
    `HdArnoldRendererPlugin` delegate are there; it renders nothing.
11. **Keep it**: `gh release create <tag>` in the private repository if it isn't there, then
    `gh release upload <tag> <zip> --clobber`.
12. **Clean up**: hosted runners are thrown away after the job, so nothing to delete.

`ARNOLD_LICENSE_ORDER` needn't be set: without a licence Arnold renders with the watermark and
the check renders nothing anyway.

## 4. Using a pack it built

On a machine with Arnold and access to the private repository:

```bash
gh release download deps/usd-70c45e83b-76ce049f --repo olihey/usdcc-arnold --pattern 'usdplugin-arnold-*-macos-arm64-py313.zip' --dir /tmp/arnold-pack
```

```bash
unzip -o /tmp/arnold-pack/usdplugin-arnold-*-macos-arm64-py313.zip -d ~/deps/usd-onetbb
```

`~/deps/usd-onetbb` stands for the OpenUSD install usdcc is built against, so the pack lands in `<OpenUSD>/plugin/arnold`
with its `usd-plugins/arnold.json`, as `build_usd_plugin.py --install` puts it. Then Settings >
Renderers > Arnold > Arnold folder, and a restart. (Optional, later: `tools/fetch_usd.py
--local-packs` doing the same from `--zips`; `fetch_usd.py` today installs only the `standard` and
published `optional` sets.)

## 5. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | The private repository's name and owner | `olihey/usdcc-arnold`, private |
| 2 | Where the SDK comes from in CI: release assets of the private repository, or Autodesk's own download for open-source CI (`forgithubci.solidangle.com`, user and password as secrets, as Gaffer's `installArnold.py` does) | Release assets now; ask Autodesk for the CI download at the same time as §6.3, and switch if they give it: then nothing of Autodesk's is stored by us |
| 3 | Whole SDKs or trimmed ones (§2) | Trimmed: less storage and less to leak; the full ones stay on your machine |
| 4 | Which Arnold the pin names: `version` is 7.5.4.0, the Mac SDK is 7.5.4.2. usdcc accepts a core of the same 7.5 that is at least `version`, so a pack built with 7.5.4.2 headers runs on 7.5.4.0 cores too in usdcc's eyes | Keep 7.5.4.0 and both SDKs as they are; bump the pin (and the Windows SDK) together at the next arnold-usd tag |
| 5 | What starts a build | By hand. Later also from `olihey/usdcc`'s `deps-usd.yml` when an OpenUSD asset is published (a `repository_dispatch` with a token that may only start that workflow) |
| 6 | Keep packs for how long | All of them, as releases: an older usdcc tag's pins name an older pack, as for the public packs |
| 7 | Cost: private repositories pay for Actions minutes beyond the plan's allowance, and macOS and Windows minutes count several times a Linux minute | Small: the build takes about a minute plus fetching OpenUSD (238 MB) and the SDK. Check GitHub's current pricing before setting it up |
| 8 | Should the pack keep `licenses/usd-plugins/arnold/arnold-sdk/EULA.txt` | Yes while it is private (it says on what terms the user's own Arnold runs). Drop it if the pack is ever published without SDK files: shipping the SDK's EULA suggests the SDK is in it |

## 6. Licences (read 2026-11-26)

1. **arnold-usd** (`LICENSE.md`): Apache-2.0, plus RodeoFX's OpenWalter under a modified Apache
   licence. Building it and shipping the binaries is allowed; ship the whole `LICENSE.md` (the
   pack does), mark changed files (the patch), and use "Arnold" only to say what it works with.
2. **Arnold SDK `EULA.txt`** (in the SDK, 7.5.4.2 for macOS): Autodesk's Terms of Use apply; no
   "ray server" mode; the SDK may be distributed only as part of a "Developed Application", only
   to its licensees, under terms no less protective of Autodesk than the Terms of Use; renders
   have a watermark. usdcc's BSD-3-Clause isn't such terms, hence §1's first decision.
3. **Autodesk General Terms of Use** (autodesk.com, updated 2026-11-11): §8.1 no distributing
   any part of an Offering; §8.3 SDKs only for "internal business use" *unless the Special Terms
   say otherwise*, which the EULA does for Developed Applications; §13(b) Autodesk owns material
   "based on, derived from, or otherwise using" its APIs. That last clause, read literally,
   covers every plugin anyone builds, which the EULA's grant suggests isn't meant: it is the one
   thing to have Autodesk confirm before the pack is published (§7).
4. **Precedent:** Gaffer (BSD-3-Clause) builds its Arnold plugins in public CI from Autodesk's
   CI download and publishes them, without the core, for 7.4 and 7.5 (release 1.7.7.0).

Not legal advice: what is above is what the texts say, read by us.

## 7. Phases

1. **Set up** (you): create the private repository, upload the two SDK archives (trimmed or not,
   §5 item 3) to its `sdk` release, note their SHA-256 in `sdk.json`.
2. **The workflow** (Claude): `arnold.yml`, `README.md`, `sdk.json` as above, and in
   `olihey/usdcc` a line in [USD_PLUGINS.md](USD_PLUGINS.md) §4 pointing here. Done when a manual
   run builds and checks both packs and they are in the private release for the current OpenUSD
   asset.
3. **Try a pack from it** on the Mac (§4) and render with `tests/renderer_viewport_check.py`
   (`RV_RENDERER=HdArnoldRendererPlugin`), as for the local build.
4. **Ask Autodesk** (you): the CI download (§5 item 2), and in the same message whether usdcc may
   publish hdArnold binaries, Apache-2.0, built against the SDK, without the core (§6.3).
5. **Only if Autodesk agrees in writing:** the pin's `set` becomes `optional` with `"publish":
   true`; the private workflow's last step also uploads the zip to `olihey/usdcc`'s OpenUSD asset
   release (a token that may only write releases there), and it ships as an optional pack
   ([USD_PLUGINS.md](USD_PLUGINS.md) §10) with a note that it needs Arnold from Autodesk. Or, with
   Autodesk's CI download, the build moves into `deps-usd-plugins.yml` itself, as Gaffer's is.
6. **Later:** Linux x64 (the SDK exists for it; the pin, the runtime's loader and a Linux check
   need it), Windows arm64 if Autodesk ships an arm64 SDK.
