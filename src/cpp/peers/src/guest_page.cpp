#include "page_server.h"

namespace usdcc::peers {

// A plain page, for a room whose feature brings no page of its own. Everything a
// member sends is shown as text, never as HTML (PEERS.md §6.1).
QByteArray builtInGuestPage() {
    return QByteArrayLiteral(R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>usdcc room</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font: 14px system-ui, sans-serif; background: #1e1f22; color: #dfe1e5; }
  main { max-width: 42rem; margin: 0 auto; padding: 16px; }
  h1 { font-size: 18px; font-weight: 600; }
  #people { color: #9da0a8; }
  #log { white-space: pre-wrap; border: 1px solid #3c3f45; border-radius: 4px; padding: 8px; height: 50vh; overflow: auto; }
  input { width: 100%; box-sizing: border-box; margin-top: 8px; padding: 6px; background: #2b2d30; color: inherit;
          border: 1px solid #3c3f45; border-radius: 4px; }
</style>
</head>
<body>
<main>
  <h1 id="title">Joining…</h1>
  <p id="people"></p>
  <div id="log"></div>
  <input id="say" placeholder="Say something" hidden>
</main>
<script>
"use strict";
const secret = new URLSearchParams(location.hash.slice(1)).get("s") || "";
let name = "";
try { name = localStorage.getItem("usdcc.room.name") || ""; } catch (e) {}
if (!name) {
  name = (window.prompt("Your name") || "Guest").trim().slice(0, 64) || "Guest";
  try { localStorage.setItem("usdcc.room.name", name); } catch (e) {}
}
const people = new Map();
const title = document.getElementById("title");
const log = document.getElementById("log");
const say = document.getElementById("say");

function line(text) {
  const div = document.createElement("div");
  div.textContent = text;
  log.appendChild(div);
  log.scrollTop = log.scrollHeight;
}
function showPeople() {
  document.getElementById("people").textContent = [...people.values()].map(p => p.name).join(", ");
}
function decode(data) {
  if (typeof data === "string") return JSON.parse(data);
  const length = new DataView(data).getUint32(0);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(data, 4, length)));
  header.bytes = new Uint8Array(data, 4 + length);
  return header;
}

const socket = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
socket.binaryType = "arraybuffer";
socket.onopen = () => socket.send(JSON.stringify({ channel: "room", type: "hello", body: { protocol: 1, secret, name } }));
socket.onclose = () => { if (!title.dataset.ended) title.textContent = "The connection was lost"; };
socket.onmessage = event => {
  const message = decode(event.data);
  const body = message.body || {};
  if (message.channel === "room") {
    if (message.type === "welcome") {
      title.textContent = body.name || "usdcc room";
      for (const peer of body.peers || []) people.set(peer.id, peer);
      showPeople();
      if ((body.channels || []).some(c => c.name === "chat")) say.hidden = false;
    } else if (message.type === "joined") {
      people.set(body.id, body); showPeople(); line(body.name + " joined");
    } else if (message.type === "left") {
      const peer = people.get(body.id); people.delete(body.id); showPeople();
      if (peer) line(peer.name + " left");
    } else if (["denied", "removed", "closed"].includes(message.type)) {
      title.dataset.ended = "1"; title.textContent = body.reason || "The room is closed"; say.hidden = true;
    }
  } else if (message.channel === "chat" && message.type === "say") {
    const peer = people.get(message.from);
    line((peer ? peer.name : "?") + ": " + String(body.text || ""));
  }
};
say.addEventListener("keydown", event => {
  if (event.key !== "Enter" || !say.value.trim()) return;
  socket.send(JSON.stringify({ channel: "chat", type: "say", body: { text: say.value.trim() } }));
  say.value = "";
});
</script>
</body>
</html>
)html");
}

}  // namespace usdcc::peers
