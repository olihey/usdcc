# Relationships in the Attribute Editor

A relationship's row shows its targets, which can be selected, removed and added, through a dialog
that greys out prims the relationship can't sensibly target. What it can target comes from a registry
that C++ and Python can change at runtime. Adding `MaterialBindingAPI` picks its material at once.

## 1. The rows

`RelationshipTargetsEditor` replaces the old read-only text and Select button.

- **Each target is a pill** showing its path, wrapping to more lines. A click selects that prim; the × removes
  that target. Each edit is one undo step, in the stage's edit target.
- **A problem is amber,** with the reason in the tooltip: the prim doesn't exist (and so can't be selected), or
  the relationship's rule objects to it.
- **The row's last button** adds a target through the dialog (§2). For a relationship holding one target
  (its rule's `single`) it's a pencil once set, and the new target replaces the old, starting from the current one.
- **Removing the last target** also removes the relationship's spec when the edit target holds its only opinion,
  so no empty `delete rel` is left in the layer. Where another layer has an opinion, a delete is what's needed.
- **`material:binding` with nothing set:** `MaterialBindingAPI` defines no properties, so a prim with the schema
  and no binding would have no row. It gets one anyway, even with Authored Only on, since an unset binding is never
  authored and this row is how one gets set.
- **The Material section** comes right after Transform, not with the namespaced sections at the end.
- **Instance proxies** are read-only: no × and no add button.

## 2. The target dialog

`RelationshipTargetDialog` shows the stage's prims in a `PrimTreeWidget`, the tree and name filter also used by
Show Prims, so it looks like the Outliner.

- **Blocked rows** are dimmed and can't be selected, with the rule's reason in the tooltip. They stay enabled, so
  they can still be expanded to reach what's under them (`/Looks` is usually a Scope).
- **Warned rows** are amber and can be picked.
- **Use Prim** or a double-click picks; **Cancel** does nothing. Esc clears the filter first.
- As in the rest of the tree, an instance's contents load when it's expanded, so until then the filter can't find them.

## 3. What a relationship can target: `RelationshipTargetRules`

USD doesn't record this. *Checked:* across the 40 relationships the schemas define, the only metadata is `custom`,
`displayName` and `variability`, and USD accepts any target: `material:binding` takes an Xform, and the bound
material then computes as nothing.

So it's a registry of ours (`relationship_targets.h`, usdcc_usd, Qt-free). The app fills it with the same `set()`
a plugin calls, so built-in rules get no special treatment.

```cpp
RelationshipTargetRule rule;
rule.relationship = "myStudio:look";   // a name, with at most one * for any text
rule.types = {"Material"};             // this prim type, or one deriving from it
rule.schemas = {};                     // or a prim with this API schema (any instance)
rule.severity = RelationshipTargetRule::Severity::Warn;  // Block: greyed out; Warn: amber
rule.single = true;                    // holds one target: another replaces it
rule.reason = "Usually a Material";    // the tooltip
RelationshipTargetRules::instance().set(rule);
```

```python
usdcc.usd.set_relationship_targets("myStudio:look", types=["Material"], severity="warn",
                                   single=True, reason="Usually a Material")
usdcc.usd.remove_relationship_targets("myStudio:look")
usdcc.usd.relationship_targets("material:binding")   # the rule that applies, or None
usdcc.usd.relationship_target_rules()                # all of them
```

- **Changes apply at once:** the dialog asks the registry when it opens, and a row each time it refreshes.
- **The same name replaces,** so running a script again doesn't pile up rules, and a built-in rule can be overridden.
- **Which applies:** the exact name, else the pattern with the most literal text; a later rule wins a tie. No rule, or one
  with no types and schemas: any prim is fine.
- **A type or schema USD doesn't know** logs a warning and is compared by name, since its plugin may load later.
- **Not saved:** rules are registered on each run, so stale ones can't linger in settings.
- **Data only, for now.** A Python function per prim would be more flexible, but it would run Python for every prim in
  the dialog. It can be added without changing these rules. Rules in a schema plugin's `plugInfo.json` are also left for later.

### Built-in rules

From each relationship's documentation in its schema:

| Relationship | Target | Holds one | Note |
|---|---|---|---|
| `material:binding`, `:full`, `:preview` | Material | yes | Not `material:binding:*`: a collection binding targets a collection as well |
| `skel:skeleton` | Skeleton | yes | |
| `skel:animationSource` | SkelAnimation | yes | |
| `skel:blendShapeTargets` | BlendShape | no | |
| `light:filters` | LightFilter | no | |
| `camera` | Camera | yes | "Must target a Camera prim" |
| `products` | RenderProduct | no | |
| `orderedVars` | RenderVar | no | |
| `inputPasses` | RenderPass | no | |
| `renderSource` | RenderSettings | yes | A warning: a pass with a `fileName` points into that file |
| `physics:body0`, `physics:body1` | Xformable | yes | |
| `physics:filteredGroups` | PhysicsCollisionGroup | no | |
| `physics:simulationOwner` | PhysicsScene | yes | |
| `boundingVolume` | Boundable | yes | |
| `proxyPrim` | Imageable | yes | |

Everything else, such as `collection:*:includes`, takes any prim. `lightList`, `portals` and `geometry` have no rule yet:
their documentation doesn't name a type precisely enough.

## 4. Adding `MaterialBindingAPI`

The + menu's entry opens the target dialog for `material:binding`, titled "Bind a Material to Box":

- **Use Prim** applies the schema and sets the binding, in one undo step (`applySchemaWithTarget()`).
- **Apply Without Material** applies only the schema; the row (§1) can bind one later.
- **Cancel** applies nothing.

Only `material:binding` is set: a purpose's binding (`:full`, `:preview`) can't be created from here yet, as it has no row
until it's authored.
