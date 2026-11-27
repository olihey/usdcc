"""Notifications (PEERS.md §10.2): the body of a `notify` message on the `notify` channel.

    {"id": "farm/8812", "kind": "success", "title": "Render finished",
     "body": "shot_012 · 240 frames", "sticky": false,
     "actions": [{"label": "Open Folder", "open": "file:///P:/shots/012/render"}]}

An action is one of a fixed list, never one that runs anything:

| Key | What a click does |
|---|---|
| `open` | Opens a URL (http, https) or a file or folder (file://), after showing it |
| `openStage` | Opens a stage, by its asset path |
| `select` | Selects prims, by path, in the current stage |
| `copy` | Copies text |
"""

from __future__ import annotations

KINDS = ("info", "success", "warning", "error")
ACTION_KEYS = ("open", "openStage", "select", "copy")
MAX_ACTIONS = 3
MAX_TITLE = 200
MAX_BODY = 2000


class NotificationError(ValueError):
    pass


def action(label: str, **what) -> dict:
    """{"label": label, <one of ACTION_KEYS>: value}."""
    if len(what) != 1 or next(iter(what)) not in ("open", "open_stage", "openStage", "select", "copy"):
        raise NotificationError("An action is one of open=, open_stage=, select= or copy=")
    key, value = next(iter(what.items()))
    return {"label": label, "openStage" if key == "open_stage" else key: value}


def check_action(value) -> dict:
    if not isinstance(value, dict) or not isinstance(value.get("label"), str) or not value["label"].strip():
        raise NotificationError("An action needs a label")
    keys = [key for key in ACTION_KEYS if key in value]
    if len(keys) != 1:
        raise NotificationError(f"An action has exactly one of {', '.join(ACTION_KEYS)}")
    key = keys[0]
    target = value[key]
    if key == "select":
        if not isinstance(target, list) or not target or not all(isinstance(p, str) and p.startswith("/")
                                                                   for p in target):
            raise NotificationError("`select` is a list of prim paths")
    elif not isinstance(target, str) or not target:
        raise NotificationError(f"`{key}` is text")
    elif key == "open" and not target.lower().startswith(("http://", "https://", "file://")):
        raise NotificationError("`open` is an http, https or file URL")
    return {"label": value["label"].strip()[:40], key: target}


def notification(title: str, body: str = "", kind: str = "info", *, id: str = "", sticky: bool = False,
                 actions: list | None = None) -> dict:
    """A notification's body, checked; NotificationError for one that isn't right."""
    if not isinstance(title, str) or not title.strip():
        raise NotificationError("A notification needs a title")
    if kind not in KINDS:
        raise NotificationError(f"`kind` is one of {', '.join(KINDS)}")
    if not isinstance(body, str):
        raise NotificationError("`body` is text")
    actions = list(actions or [])
    if len(actions) > MAX_ACTIONS:
        raise NotificationError(f"At most {MAX_ACTIONS} actions")
    result = {"kind": kind, "title": title.strip()[:MAX_TITLE]}
    if body:
        result["body"] = body[:MAX_BODY]
    if id:
        if not isinstance(id, str) or len(id) > 128:
            raise NotificationError("`id` is text of at most 128 characters")
        result["id"] = id
    if sticky:
        result["sticky"] = True
    if actions:
        result["actions"] = [check_action(each) for each in actions]
    return result


def from_json(value: dict) -> dict:
    """A notification from JSON (the hub's POST /notify), checked."""
    if not isinstance(value, dict):
        raise NotificationError("The body isn't a JSON object")
    return notification(value.get("title", ""), value.get("body", ""), value.get("kind", "info"),
                        id=value.get("id", "") or "", sticky=bool(value.get("sticky", False)),
                        actions=value.get("actions"))
