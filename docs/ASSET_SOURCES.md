# Asset Sources (plan)

Several asset configurations at once: the user's own, one from studio A, one from studio B. With
them, logins that can't go to the wrong studio, configurations from a web service, logins in the
Asset Servers dialog, and the environment in the status bar. Builds on
[REMOTE_ASSETS.md](REMOTE_ASSETS.md) (the configuration, §4; the dialog, §10.1).

**Status:** planned and its questions decided 2026-11-24 (§13). **Phases 1 to 5 are built**
(§10.1 to §10.5; macOS's store isn't). "Checked" means read in this repository's code; everything else is an assumption
or a proposal.

## 1. Decisions

- **Each file is a *source* with a local alias,** and what it defines is qualified by it
  (`studio-a/prod`). Sources are never merged silently.
- **USD files never contain an alias.** `webdav://server1/a.usd` stays as the studio wrote it.
- **Only the personal file can be edited.** Every other source is read-only in usdcc.
- **A stage uses one environment,** from one source. Mixing sources is explicit, in a personal
  environment (§4).
- **The environment is per stage,** with a global default for new ones (§8). It's so today.
- **A configuration carries no secrets** (§6).
- **A login belongs to a source,** and adding a source is trusting it with its own logins (§5).

## 2. What was there before

*Checked:*

- **One file:** `USDCC_ASSETS_CONFIG`, or else the user's `assets.json` (`Config::defaultPath()`).
- **The environment** of a new stage is Open Stage with Options' choice, else Settings > Assets >
  Environment, else `USDCC_ASSET_ENV`, else the file's. Sessions save each stage's.
- **A login is found by its credential id alone,** whichever server names it (`credentialFor()`).
- **The login dialog's dropdown** lists the ids the configuration names, not the logins saved in
  Windows: one saved under any other id doesn't show up.
- **Saving with an empty password field** replaces the saved password with an empty one. The field
  always starts empty.
- **Logins can be saved on Windows only.** Elsewhere there are the environment variables.

## 3. Sources

The personal file lists the others:

```json
{
  "sources": [
    { "alias": "studio-a", "path": "//nas/pipeline/assets.json" },
    { "alias": "studio-b", "url": "https://studio-b.example/usdcc/assets.json" }
  ]
}
```

- **The alias** is suggested by the source (`"name"` in its file), else made from its file or host
  name. The user is asked only on a clash, and can change it. It has the characters of a name
  (REMOTE_ASSETS.md §3), so no `/`.
- **The personal source has no alias.** Its ids stay bare, so today's configurations, sessions and
  saved logins keep working.
- **Read-only:** usdcc never writes another source's file, writable or not. Its servers can be
  tested and logged in to; the source can be refreshed, turned off and removed. The editor today
  writes `USDCC_ASSETS_CONFIG`'s file when it can: that stays, for someone who maintains it.
- **Only the personal file** decides `offline` and `cacheLimitGB`, and may have `sources`,
  `mixed` (§4) or ids with an alias. In any other source they're ignored, or a problem of it.
- **A broken source** is ignored with a warning, and the others still work.
- **`USDCC_ASSETS_CONFIG`'s file** takes the personal file's place, as today, and may have `sources`
  of its own: a farm gets several studios through one file. Relative paths in it are to that file.

**Where an alias appears:**

| Thing | Where | Alias |
|---|---|---|
| Name, `webdav://server1/…` | USD files | Never |
| Environment, `studio-a/prod` | Status bar, sessions, jobs, `USDCC_ASSET_ENV`, `extends` | Yes |
| Server id, `studio-a/nas` | Asset Servers, personal stacks | Yes |
| Login, `studio-a/dav` | Logins tab, Credential Manager | Yes |

**In the code:** sources are qualified as they're loaded, into one `Config`. Endpoint ids,
environments and credential ids get their prefix, so `Context`, the stacks, lookups and the cache
should need no change. `ConfigEditor` already refuses `/` in ids, but `Config::parse()` doesn't:
it has to, or a file could spell another source's ids. `Config::toJson()` keeps keys it doesn't
know, so an older usdcc leaves `sources` alone.

## 4. Names and environments

| Collision | Handling |
|---|---|
| Server or environment id | Can't happen: qualified |
| Credential id | Can't happen: qualified |
| Alias | Refused when the source is added; the user picks another |
| `webdav://studio` in two sources | The stage's environment decides |
| A web host's login in two sources | Personal first, then the list's order; shown as a warning |

**A studio's environment is exactly what the studio defined.** Personal names aren't added to it,
so a stage resolves the same for everyone using that environment.

**Mixing** is a personal environment:

```json
"mixed": {
  "mine": {
    "extends": ["studio-a/prod", "studio-b/prod"],
    "webdav": { "studio": ["my-nas", "studio-a/nas"] }
  }
}
```

- It has the names of what it extends, and its own, which replace them.
- A name two extended environments both define, and it doesn't, is an error, not a guess.
- A cycle is an error.
- An `extends` whose source is gone or turned off is a problem of that environment only.

Mixed environments have a top-level key of their own, which an older usdcc keeps and ignores. Inside
`"environments"` it would refuse the whole file, as it would a server id it doesn't know. For the
same reason a qualified or mixed default goes in a new key, `"defaultEnvironment"`, which wins over
`"environment"`. Names of both kinds share one list, so a clash is a problem.

**A name the stage's environment doesn't have.** A studio's USD file uses `webdav://server1/…`, its
source is added, but the default environment is personal:

| Situation | What usdcc does |
|---|---|
| The environment has `server1` | Uses it |
| It doesn't, and one source does | Offers "server1 is in Studio A. Reopen in studio-a/prod?" |
| Several sources do | Asks which: the USD file can't say |
| None does | Says it's missing, and offers **Asset Servers…** |

- **The environment offered** is the source's default when it has the name, else the only one that
  does, else a choice.
- **Offered, never switched silently:** a name in studio A's file doesn't prove the scene is
  theirs, and switching contacts their servers. It's a toast and never a question from a resolver
  thread, for the reason REMOTE_ASSETS.md §10 gives for logins.
- **Remembered** for the stage's folder, in the app's settings, so it's one question per project.
- **Scripts and jobs** have nobody to ask and are given the environment (§8).

## 5. Logins

**The problem.** A login is found by its id alone. With several sources, studio B's file could name
studio A's credential id beside a URL of B's, and be sent A's password.

- **Per source:** a studio's credential id becomes `<alias>/<id>`, saved as
  `usdcc-assets:studio-a/dav`. A source can't name a login of another. Personal ids stay bare.
- **Adding a source trusts it** with the logins saved for it: its file may change the servers, mirrors
  and web hosts they go to, and nothing is asked. The risk accepted: a studio's changed or broken-into
  file can redirect *that studio's* logins, never another's or a personal one.
- **`aws` and `aws:<profile>`** are the user's own AWS login and aren't qualified. A source may name
  them. **Add…** says when it does.
- **Environment variables:** `USDCC_ASSETS_<ID>_…` writes everything but letters and digits as `_`,
  so `studio-a/dav` and a personal `studio-a-dav` share one. Two configured ids with one spelling
  are a problem, and neither gets the variable.
- **Removing a source** offers to forget its logins. Adding one under an alias that still has logins
  asks whether it may have them: it may be another studio's.
- **Allowed hosts and buckets** of a source, and `insecure` hosts, are shown when it's added and
  accepted with it, later changes too. They apply to the whole process, as direct URLs have no source.
  A bare `*` is ignored: it would turn the check off for every stage.
- **Linux** has the desktop's keyring (§10.5). **macOS** would need its Keychain before this is
  practical there.

## 6. Can the configuration carry credentials?

**No secrets.** The file is shared, cached and backed up; a secret in it can't be revoked for one
person, and would be read by every tool. Instead:

1. **A description,** so the login form asks for the right thing:

   ```json
   "credentials": {
     "dav": { "label": "Studio A files", "kind": "password",
              "help": "https://studio-a.example/usdcc-access" }
   }
   ```

   `kind` is `password`, `token` or `s3`. The help link opens only on a click.
2. **Later, a token service:** the source names an HTTPS address, the user logs in there once, and
   it gives out short-lived personal tokens, or temporary S3 keys. They're kept in memory only;
   `Credential::expires` is there already.
3. **Later still, single sign-on** through the browser (OIDC).

## 7. A configuration from a web service

A URL in `sources`. A static file on a web server is enough; nothing usdcc-specific runs there.

- **HTTPS only.** An optional login, `<alias>/@source`, saved as any other. Redirects stay
  on the host.
- **Refreshed** by ETag at start, hourly and on **Refresh**, always in the background. Opening a
  stage never waits for it.
- **The last valid copy** is kept in the user's data folder and used when the service is down, the
  machine is offline or the new text doesn't parse. The dialog says how old it is and why.
- **A change** applies to stages opened afterwards, as a save in the editor does.
- **Other USD tools** read the last copy. With none (a new farm machine) the library fetches it once
  with a short timeout, for an anonymous source or one whose login is in the variables.
- **Not signed** at first: HTTPS is the protection.

## 8. The environment of a stage

**Per stage, with a global default.** Test and prod can be open side by side, and sessions already
keep each stage's. `assetEnvironment` holds the qualified name; a bare one is personal.

**Restoring an environment that's gone** opens the stage in the default today, silently. With
sources that could be another studio's: it should say so in a toast that offers the choice.

**Renders and validation.** *Checked:*

Phase 2 changed this (§10.2). Before it:

| Path | Environment |
|---|---|
| Render from the app, on this machine | The stage's: the job flattens and opens its copy with its context |
| A scene bundle sent to another machine | The stage's: assets are gathered with it bound |
| A shared scene (dependencies on a share) | **The other machine's default:** `SharedScene` doesn't carry one, and it's opened with the default context |
| `usdcc_node render` and `validate` | `USDCC_ASSET_ENV` or the file's: there is no option |

So `SharedScene` gets the environment, and the two commands `--asset-env`. A machine that doesn't
have the named environment fails the job, naming it, rather than render other files. That includes
one whose alias for the same studio differs.

**Peers.** Joining a shared stage uses the host's environment when the participant has one of that
name; otherwise their default, with a warning naming both. A shared stage isn't switched (§10.2).
An equal name is taken as equal; the stacks' ids could check it later.

## 9. The status bar

A button among the permanent widgets, beside the layer being edited: the **current stage's**
environment (`studio-a / test`), highlighted when it isn't the default. Hidden when no environment
is configured.

Its menu:

- The environments, grouped by source, the current one checked.
- **Use for All Open Stages**
- **Use for New Stages** (Settings > Assets > Environment)
- **Asset Servers…**

With no stage it shows the default, and picking one sets it.

**Picking another environment asks** whether to reload the stage now: **Reload**, **Later**, **Cancel**.

- **Later** keeps it for this stage, and it applies when the stage is next reloaded or reopened. Until
  then the status bar shows both (`prod → test`), and everything resolves in the first. The pick is
  undone by picking the stage's own environment.
- **A session** saves the pending one, as restoring reopens the stage. Renders use the actual one.
- **Use for All Open Stages** asks once for all of them.
- Reloading single layers isn't reopening and doesn't apply it.

**Reload reopens the stage.** usdcc gives a stage its context when it opens, and a `UsdStage`
has no call to change it. Reopening goes through what sessions save (`session_stages.cpp`):
selection, edit target, variant edit, time, muted layers, load rules and session layer, plus what
the panels keep (cameras).

- **Unsaved layers** are asked about first (save, discard, cancel). A server's layer is another
  layer in the other environment, so its edits wouldn't come along.
- **The undo history** of the stage is lost, and the question says so.
- **A stage in memory** can't be reopened from a file; it's carried over as a session does.
- **If opening fails,** the old stage stays.

**Spike:** whether a context that refers to its environment, with `ArNotice::ResolverChanged`,
switches a stage in place. Contexts are compared and hashed by value, and resolved paths carry the
stack's id, so layers would have to be replaced by their counterparts. Nothing depends on it.

## 10. The dialog

```
+--------------------------------------------------------------------------+
| Sources          | [Servers] [Environments] [Web Hosts] [Logins]          |
|  Personal        |                                                        |
|  Studio A  (ro)  |   what the selected source defines                     |
|  Studio B  (ro)! |                                                        |
| [Add...] [Remove]|                                                        |
| [Refresh]        |                                                        |
+--------------------------------------------------------------------------+
```

- **Sources:** **Add…** takes a file or a URL, and shows what the source defines, its allowed hosts
  and buckets before it's added. Each shows where it comes from, its state and its age.
- **Read-only sources** look the same with their fields off. **Copy to Personal** copies a server or
  an environment, and replaces **Save a Copy as Mine**. A copied server loses its credential: a
  personal one is picked.
- **Servers** get a login column: logged in, no login, refused (by the last Test).
- **Logins** lists every saved login, the ids the sources name without one, and those found only in
  variables: its source, kind, user, status, and the servers using it. One nothing uses says so.
  **Log In…**, **Change…**, **Forget** (which asks, naming the servers). Never a secret.
- **The credential column** offers the known ids of its source.
- **An empty password on Save keeps the saved one.** It's refused for a new login.
- **Edit > Asset Server Login…** goes; the picker keeps **Log In as…**, with the same form.

### 10.1 Phase 1 as built

Built 2026-11-24, for the one configuration there is:

- The **Logins** tab, the servers' Login column, the credential cells' list and the empty-password
  rule, as above. Edit > Asset Server Login… is gone. Details: [REMOTE_ASSETS.md](REMOTE_ASSETS.md)
  §10.2.
- **Login states of a server:** Saved, Variables, AWS, No login, or Refused by the last Test with
  that credential. Saving any login clears Refused.
- **Found only in variables:** shown for ids the configuration names. Variables can't be turned back
  into ids, so others aren't listed.
- **A web host without a credential** is listed as a use when a login is saved under its address, and
  isn't listed as missing one otherwise.
- **A login nothing uses** says so; its tooltip says a server opened by that address would use it.
- **Add Login…** starts with an empty id. A user is required: one without is stored as a token.

### 10.2 Phase 2 as built

Built 2026-11-24, as §8 and §9 have it, but for what follows. The code is in
[asset_environment.h](../src/cpp/usd/include/usdcc/usd/asset_environment.h): `reopenInEnvironment()`
and the status bar's `AssetEnvironmentButton`.

- **What's pending** is the stage's (`Stage::pendingAssetEnvironment()`). The menu gets **Reload in
  <environment> Now** while there is one. Nothing else reloads a whole stage, so that's where Later
  is applied, besides a session.
- **Sessions** save `"pendingAssetEnvironment"` beside `"assetEnvironment"`, and restore in the
  pending one. One that isn't configured any more says so in a toast, and the stage opens in the
  default.
- **Reopening** opens the new stage on a worker, behind a modal progress dialog, while the old one
  stays open. Only then does it ask about unsaved layers, and only those the new stage doesn't use:
  a file both use keeps its edits. Cancel keeps the old stage. The new stage takes the old one's
  place as Save Stage As does (`replaceStage()`, which both use now).
- **Save Stage As** keeps the stage in its environment, and what's pending stays pending.
- **A layer is saved in a session by its path,** not by a server's resolved path, whose stack id
  only this environment has. The edit target is found in the stage's own context.
- **New stages** in memory and new files open in the default environment too (before, in the
  library's, without Settings > Assets' choice).
- **A shared stage isn't switched:** reopening would end the share. Its environments are off in the
  menu, with the reason, and **Use for All Open Stages** leaves it as it is. Decided by the user
  2026-11-24; what §8 has the host's switch do isn't built.
- **Peers:** the shared stage's info carries the host's `assetEnvironment`. Joining opens the root,
  and looks up its layers, in it when it's configured here. A stage opened in another one gets a
  toast naming both, unless no environment is configured here.
- **Shared scenes** (renders on peers) carry `assetEnvironment` only when a dependency is on a named
  server; dependencies are listed by their logical path. A peer without that environment lists it
  as a problem, so the job goes by bundle instead. *Changed from §8:* it isn't failed, since the
  bundle has the files.
- **`usdcc_node render` and `validate`** take `--asset-env <name>`; one that isn't configured is a
  usage error (exit 2).
- **Tests:** `asset_environment_switches_by_reopening` ([current_stage_test.cpp](../tests/current_stage_test.cpp))
  and a shared scene with an unknown environment in `render_network_bundle_and_shared`. The reopening
  test has no panel: an Outliner made and destroyed without being shown makes the UI scale tests
  that run later fail (*not looked into*).

### 10.3 Phase 3 as built

Built 2026-11-24, as §3 to §5 and §10 have it, but for what follows. The library's part is in
[sources.h](../src/cpp/assets/include/usdcc/assets/sources.h): `loadSources()` and
`mergeSources()`, which make the one `Config` everything else reads.

- **One merged configuration.** `Config::loadDefault()` reads the personal file, then its sources,
  and merges them: servers, environments and credential ids of a source with its alias in front,
  its web hosts and allowed lists added, its mixed environments and its own `sources` left out
  (and said so). Contexts, stacks, lookups, the cache and logins work on the merged one as they did.
  The editor keeps the personal file apart, as that's what it writes.
- **`"mixed"`** is read into the same map as plain environments, with `"extends"` beside it. An
  environment is written there when it extends one or a stack names a source's server; the default
  goes in `"defaultEnvironment"` when it's one of those, or a source's.
- **A source that can't be read** is said in the Log and marked in the dialog; a problem in the
  personal file keeps it from being saved. Only a source's environments can be extended: a mixed
  one extending another isn't allowed, so there are no cycles to find.
- **A bare `*`** in a source's allowed lists isn't taken. A web host two configurations have uses the
  first one's login; the other is said.
- **Two credential ids with one spelling of environment variables** (a personal `studio-a-dav` and a
  source's `studio-a/dav`): neither reads them, and the merge says so.
- **`"name"`** in a file suggests its alias; else its file name made into one. Parsing refuses `/` in
  ids, so a file can't spell another source's. A source's default environment is `sourceDefaults`
  in the merged configuration.
- **The dialog** has the configurations on its left: **Mine** and each source, read-only. **Add…**
  asks for an alias only when its own is taken, and says what comes with it: its servers, the hosts
  and buckets it allows, whether it uses your AWS login, and logins already saved under its alias.
  **Remove** offers to forget those. **Copy to Mine** copies a server (without its login) or an
  environment, which becomes a mixed one on the source's servers. A mixed environment's extended
  environments are under its name in Environments. Logins are the merged configuration's.
  *Not built:* renaming an alias (remove it and add it again), and turning a source off.
- **The offer** (`installAssetEnvironmentOffer()`): the resolver tells the app (`setUnknownNameCallback()`,
  from any thread) about a name its environment lacks. The app shows a sticky toast once per name
  and environment, with a button per environment to reopen the stage in it: a source's default when
  it has the name, else each of its environments that has, and personal ones. With none, it says
  so and offers the Asset Servers editor; without any environment configured, only the Log says.
  Set up before the stage named on the command line opens, so it's offered for that one too.
- **Remembered per folder** (`asset-folders.json` in the app's local data; a tool or test without one
  remembers nothing): what the offer reopened a stage in, or **Use <environment> for This Folder** in
  the status bar's menu. Switching by itself remembers nothing. Stages in that folder and below
  open in it, the nearest folder's first, also in Open Stage with Options; one no longer configured
  is passed over. The button's tooltip says so, and **Forget <environment> for This Folder** takes
  it back.
- **The status bar's menu** groups environments by source: yours, then each source's under its
  alias.
- **Tests:** `assets_sources_are_named_with_their_aliases` ([assets_test.cpp](../tests/assets_test.cpp)),
  `asset_servers_editor_shows_and_mixes_sources` and `asset_folders_remember_their_environment`
  ([asset_servers_editor_test.cpp](../tests/asset_servers_editor_test.cpp)). The app's offer was
  checked by hand: a stage naming a studio's server opens in the personal environment, and the
  toast reopens it in the studio's.

### 10.4 Phase 4 as built

Built 2026-11-24, as §7 has it, but for what follows. The library's part is `refreshSource()` and
`loadSource()` in [sources.h](../src/cpp/assets/include/usdcc/assets/sources.h), the app's is
[asset_source_refresh.h](../src/cpp/usd/include/usdcc/usd/asset_source_refresh.h).

- **The URL** is `https://` with no login in it. Redirects are followed five times at most, on the
  same scheme, host and port. 30 seconds for the whole request, 8 MB at most.
- **The copy** is one file per alias and URL, with the text, its ETag and when it was checked and
  changed, in `asset-sources` in the user's usdcc data folder, or the folder `USDCC_ASSET_SOURCES`
  names. Written through a temporary file. A server may answer each login differently, so two
  aliases of one URL have two.
- **Taken only if it parses and has something in it:** any JSON object parses, and a server's error
  message as JSON would have replaced the copy with nothing.
- **Loading reads the copy.** Tools fetch one that has none, once, for 5 seconds at most, and not
  when offline. The app turns that off (`setFetchSourcesAtLoad()`): a stage opened at the first start
  with a new source may not find its names until the fetch is done and the stage is reopened.
- **The app** refreshes at start and hourly, not while offline, on a worker. A change reads the
  configuration again and fires `assetConfigChanged()`. Failures go to the Log.
- **The dialog:** **Add...** asks for a file or a URL. A URL is first asked without any login, under an
  alias made from its host, as one saved under that alias may be another server's. A refusal opens
  the login form, and the login moves to the alias the file suggests. **Refresh** asks the servers,
  behind a progress dialog. A source's page says when it was checked and changed and why a check
  failed; **Source Login...** is its own login. Removing one removes its copy once that's saved.
- **In the Logins tab** `<alias>/@source` is listed when it's saved, with the source as its use.
- **Save** works whichever configuration is shown. In phase 3 it did nothing while a source was.
- **Tests:** `assets_source_urls_are_https`, `assets_source_from_a_url_is_kept` and
  `assets_source_logins_stay_with_their_server` ([assets_net_test.cpp](../tests/assets_net_test.cpp)),
  and `asset_servers_editor_adds_a_source_from_a_url`. They use the tests' plain HTTP server, which
  `allowLoopbackSources()` lets through on 127.0.0.1. HTTPS was checked by hand.

### 10.5 Phase 5 as built

Built 2026-11-24, for Linux: `SecretServiceStore` in
[secret_service.cpp](../src/cpp/assets/src/secret_service.cpp).

- **Through libsecret,** to whatever answers as the Secret Service (GNOME Keyring, KWallet,
  KeePassXC). It's loaded when first needed (`dlopen`, its and GLib's stable C functions declared
  here), so neither the build nor the packages depend on it. Without it, without a session bus, or
  with `USDCC_ASSETS_KEYRING=0`, there is no store: logins come from the environment variables.
- **An item** has the schema `org.usdcc.Assets`, the attributes `id` and `user` (empty for a token)
  and the secret. Saving another user's removes the one before, once the new one is there.
- **Listing** reads attributes only, so it never asks to unlock. Reading and saving may, through the
  desktop's own dialog, on whichever thread asks, which waits: one thread at a time asks. A login
  kept in memory for 30 seconds, missing ones too, so a refused question isn't asked for every
  request.
- **Not macOS.** Written before usdcc built there. It does now, so its Keychain can be
  built and tried now; until then logins there come from the environment variables.
- **Peers' identity key** stays in its file (PEERS.md); this is only the assets' logins.
- **Tried** on Ubuntu 24.04 (WSL) with gnome-keyring 46 and libsecret 0.21 in a session bus of their
  own: the test `assets_credentials_in_the_system_store` with `USDCC_TEST_KEYRING=1`, which only
  runs when asked as it writes to the keyring it finds; a keyring read again after a restart; and
  that nothing waits when there's no service, or when it's locked with nobody to ask. *Not tried:*
  a desktop's unlock dialog, KWallet, KeePassXC.

## 11. Phases

| # | What | Needs |
|---|---|---|
| 1 | (built) Logins: the list of saved ones, the tab, the dropdown and the empty-password fix | |
| 2 | (built) The status bar, switching by reopening, the environment in shared scenes and `--asset-env` | |
| 3 | (built) Sources from local files: aliases, read-only, `extends`, logins per source, the offer for an unknown name | 1 |
| 4 | (built) Sources from a URL | 3 |
| 5 | (built for Linux) Credential stores on Linux and macOS | 1 |
| 6 | Credential descriptions, then the token service, then single sign-on | 3 |

Phase 3 doesn't ship without logins per source: sources without them are the leak of §5.

## 12. Testing

- **`usdcc_assets`:** qualifying, two sources with the same ids and names, `/` refused, `extends`
  with clashes and cycles, a broken source beside a good one, old files unchanged.
- **Logins:** a source naming another's id, or a personal one, sends no `Authorization` header (the
  test server sees it); a removed source's alias used again; the same spelling of two variables.
- **Web source:** 304, broken text, unreachable with and without a copy, a login.
- **Reopening:** a pending environment and its session, what §9 lists is kept, and a failed open keeps the old stage.
- **The dialog:** read-only sources, Logins with entries of its own prefix only. Tests mustn't touch
  the user's real logins, so the store needs a replaceable backend.

## 13. Decided questions

Decided by the user 2026-11-24, but for 8.

| # | Question | Decision |
|---|---|---|
| 1 | Names from several sources in one stage? | Only explicitly, in a mixed environment (§4) |
| 2 | Who chooses the alias? | The studio suggests it; the user can change it (§3) |
| 3 | How far are studios' files trusted? | Adding one trusts it with its own logins; no approval per host (§5) |
| 4 | Reopening when the environment is switched? | Ask: reload now or later. Later applies on the next reload (§9) |
| 5 | Different environments in a shared stage? | Follow the host's when joining, else warn; no switching while it's shared (§8) |
| 6 | May `USDCC_ASSETS_CONFIG`'s file have sources? | Yes (§3) |
| 7 | A job's environment is unknown on a machine? | The job fails (§8) |
| 8 | Where does `extends` go, so that an older usdcc still reads the file? | Under a new top-level key, `"mixed"` (§4). A technical choice, not asked |

## 14. Not planned

- **Writing a studio's file** from usdcc, beyond what `USDCC_ASSETS_CONFIG` allows today.
- **Secrets in a configuration,** encrypted or not.
- **Aliases in USD paths** (`webdav://studio-a/studio/…`): files would depend on each user's aliases.
- **A name resolved by trying every source in turn:** what it finds would depend on their order, and
  each would be asked for the others' paths.
