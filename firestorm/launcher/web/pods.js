// The Pods page: the centre's pods as the console's launcher finds them on
// the network (GET /api/pods), and what it tells them (POST /api/pods/cmd,
// which passes it on to each pod's agent - launcher/podagent.py).
"use strict";
let PODS = null;
const STATE_NAME = { starting: "Starting", switching: "Changing over", ready: "Ready", battle: "In a match",
  restarting: "Restarting the game" };

function podsOn() { return $("#page-pods").classList.contains("on"); }

async function loadPods() {
  const r = await api("/api/pods").catch(() => null);
  if (!r) return;
  PODS = r;
  renderPods();
}

function renderPods() {
  const r = PODS, pods = r.pods || [];
  const game = r.console ? "fs" : r.battle ? "bt3" : "";
  $("#game-state").innerHTML = game ? `<span class="pill live">${game === "fs" ? "Firestorm" : "3.0 battle"}</span>`
    : '<span class="pill ok">Ready</span>';
  $("#game-fs").className = "btn " + (game === "fs" ? "primary" : "");
  $("#game-bt3").className = "btn " + (game === "bt3" ? "primary" : "");
  $("#game-bt3").hidden = !r.bt3;
  $("#game-ready").hidden = !game;
  $("#game-hint").textContent = game === "fs" ? "The console is up: the pods in service run Firestorm and wait for its LOAD."
    : game === "bt3" ? "A 3.0 battle is on, with the 3.0 page's Battle settings. The pods go back to Ready when it ends."
    : "Nothing running: the pods show Ready. Starting one game closes the other first.";
  $("#pod-grid").innerHTML = pods.map(p => {
    const off = !p.online;
    const st = off ? "Not answering" : (STATE_NAME[p.state] || p.state);
    const cls = off ? "bad" : p.state === "battle" ? "live" : p.state === "ready" ? "ok" : "";
    const note = off ? `Last heard ${Math.round(p.age)} s ago.`
      : p.mode === "ready" ? "Out of service: it shows Ready, whatever this PC plays."
      : r.console && !p.listed ? "Not in the running console's list: Firestorm again (Back to Ready, then Firestorm) adds it."
      : p.state === "battle" ? (p.fs_live ? "In a Firestorm match." : "In the 3.0 battle" + (p.detail ? ", " + p.detail : "") + ".")
      : r.console && p.fs_live && p.state === "ready" ? "Firestorm is up: waiting for the console's LOAD."
      : p.state === "ready" ? "Plays this PC's next game: Firestorm, or a 3.0 battle" +
          ((p.games || []).includes("bt3") ? "." : " (3.0 is not installed on it).") : (p.detail || "");
    const b = (mode, label) => `<button class="btn ${p.mode === mode ? "primary" : ""}" data-ip="${esc(p.ip)}" data-mode="${mode}" ${off ? "disabled" : ""}>${label}</button>`;
    return `<div class="pod-card ${off ? "off" : ""}">
      <div class="pod-top"><div class="pod-num">${esc(p.num || "")}</div>
        <div><div class="pod-name">${esc(p.name)}</div><div class="hint mono">${esc(p.ip)} - ${esc(p.host || "")}</div></div>
        <span class="pill ${cls}">${esc(st)}</span></div>
      <div class="pod-mode">${b("auto", "In service")}${b("ready", "Out of service")}</div>
      <div class="hint pod-note">${esc(note)}</div>
      <div class="pod-tools">
        <button class="btn ghost" data-ip="${esc(p.ip)}" data-cmd="/identify" ${off ? "disabled" : ""}>Identify</button>
        <button class="btn ghost" data-ip="${esc(p.ip)}" data-cmd="/settings" ${off ? "disabled" : ""}>Settings</button>
        <select data-ip="${esc(p.ip)}" class="pod-more" ${off ? "disabled" : ""}><option value="">More...</option>
          <option value="restart">Restart its game</option><option value="reboot">Restart the PC</option>
          <option value="shutdown">Shut the PC down</option></select>
      </div></div>`;
  }).join("") || `<div class="card"><p class="hint">No pods found yet. A pod shows here once it is set up (its first-time setup, as a pod) and running on this network. Windows Firewall must let the centre's programs through on both PCs - the setup's Network step does it.</p></div>`;
  $("#pods-hint").textContent = pods.length ? `${pods.filter(p => p.online).length} of ${pods.length} answering.` : "";
  $$("#pod-grid [data-mode]").forEach(el => el.addEventListener("click", () => podCmd([el.dataset.ip], "/mode", { mode: el.dataset.mode })));
  $$("#pod-grid [data-cmd]").forEach(el => el.addEventListener("click", () => podCmd([el.dataset.ip], el.dataset.cmd, {})));
  $$("#pod-grid .pod-more").forEach(el => el.addEventListener("change", () => {
    const v = el.value, ip = el.dataset.ip;
    el.value = "";
    const name = (pods.find(p => p.ip === ip) || {}).name || ip;
    if (v === "restart") podCmd([ip], "/restart", {});
    if (v === "reboot" && confirm(`Restart ${name}'s PC?`)) podCmd([ip], "/power", { action: "restart" });
    if (v === "shutdown" && confirm(`Shut ${name}'s PC down? It needs its power button to come back.`)) podCmd([ip], "/power", { action: "shutdown" });
  }));
}

async function podCmd(ips, path, body) {
  status("Telling " + ips.length + (ips.length === 1 ? " pod" : " pods") + "...");
  const r = await api("/api/pods/cmd", { ips, path, body }).catch(e => ({ error: String(e) }));
  status(r.message || r.error || "", false, !!r.error);
  setTimeout(loadPods, 300);
}

$$(".pods-all [data-all]").forEach(el => el.addEventListener("click", () => {
  const ips = ((PODS && PODS.pods) || []).filter(p => p.online).map(p => p.ip);
  if (!ips.length) return status("No pod is answering.", false, true);
  const a = el.dataset.all;
  if (a === "identify") podCmd(ips, "/identify", {});
  else podCmd(ips, "/mode", { mode: a });
}));
// The game: whichever this PC starts, the pods in service play; starting
// one closes the other first (the launcher does, fslauncher.py).
function gameNow() { return PODS ? (PODS.console ? "fs" : PODS.battle ? "bt3" : "") : ""; }

async function gameCmd(path, body, text) {
  status(text);
  $$(".game-btns .btn").forEach(b => b.disabled = true);
  const r = await api(path, body).catch(e => ({ error: String(e) }));
  $$(".game-btns .btn").forEach(b => b.disabled = false);
  status(r.message || r.error || "", false, !!r.error);
  loadPods();
}

$("#game-fs").addEventListener("click", () => {
  const now = gameNow();
  if (now === "fs") return status("Firestorm is already up.");
  if (now === "bt3" && !confirm("End the 3.0 battle and start Firestorm?")) return;
  gameCmd("/api/start", { what: "console" }, "Starting Firestorm...");
});
$("#game-bt3").addEventListener("click", () => {
  const now = gameNow();
  if (now === "bt3") return status("The 3.0 battle is already on.");
  if (now === "fs" && !confirm("Close Firestorm (a match on now ends) and start a 3.0 battle?")) return;
  gameCmd("/api/bt3/start", {}, now === "fs" ? "Closing Firestorm, then starting the 3.0 battle..." : "Starting the 3.0 battle...");
});
$("#game-ready").addEventListener("click", () => {
  const now = gameNow();
  if (!now || !confirm(now === "fs" ? "Close Firestorm? A match on now ends." : "End the 3.0 battle?")) return;
  if (now === "fs") gameCmd("/api/start", { what: "console_stop" }, "Closing Firestorm...");
  else gameCmd("/api/bt3/stop", {}, "Ending the 3.0 battle...");
});

setInterval(() => { if (podsOn() && !document.hidden) loadPods(); }, 1500);
window.addEventListener("hashchange", () => { if (location.hash === "#pods") loadPods(); });
if (location.hash === "#pods") loadPods();
