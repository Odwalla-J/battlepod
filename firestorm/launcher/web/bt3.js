// Firestorm launcher: BattleTech 3.0 - the 1990s pod on battlepod's emulator.
// Through /api/bt3 (launcher/bt3.py): the release, the battle's choices,
// the cockpit's window and controls (battletech.ini), and the battle itself.
"use strict";

// ------------------------------------------------------------ which game
const GAMES = { fs: { title: "BattleTech<br>Firestorm", home: "pc" }, bt3: { title: "BattleTech<br>3.0", home: "bt3" } };
function gameOf(page) {
  const a = $(`.nav[data-page="${page}"]`);
  return a ? a.dataset.game || null : null;
}
function setGame(g, keepPage) {
  if (!GAMES[g]) g = "fs";
  document.body.dataset.game = g;
  $$(".gameswitch button").forEach(b => b.classList.toggle("on", b.dataset.game === g));
  $(".brand .title").innerHTML = GAMES[g].title;
  try { localStorage.setItem("game", g); } catch (e) { /* a private window: fine */ }
  const page = location.hash.slice(1);
  if (!keepPage || !page || (gameOf(page) && gameOf(page) !== g)) location.hash = GAMES[g].home;
}
$$(".gameswitch button").forEach(b => b.addEventListener("click", () => setGame(b.dataset.game)));
(() => {
  let g = "fs";
  try { g = localStorage.getItem("game") || "fs"; } catch (e) { /* none kept */ }
  const page = location.hash.slice(1);
  if (gameOf(page)) g = gameOf(page);
  setGame(g, true);
})();

// ------------------------------------------------------------ state
let B = null;            // /api/bt3, as last read
let B3 = {};             // the settings, as edited
const BT3_COLOURS = ["#ff9a3a", "#4ad0ff", "#7dff6a", "#ff5a8a", "#ffe14a", "#b88cff", "#5affd8", "#ff7a5a"];

async function loadBt3() {
  // The monitors and 'Mech pictures come with the launcher's own state.
  for (let i = 0; i < 100 && !S; i++) await new Promise(r => setTimeout(r, 50));
  B = await api("/api/bt3");
  if (!B.available) {
    $("#bt3-error").hidden = false;
    $("#bt3-error").textContent = "battlepod's emulator is not with this launcher: BattleTech 3.0 needs the battlepod repository (make && make cockpit) or a packaged copy.";
    return;
  }
  B3 = Object.assign({}, B.settings);
  renderBt3();
}
function vehicleName(id) {
  const v = (B.vehicles || []).find(v => v.id === +id);
  return v ? v.name : "Mech " + id;
}
// The 3.0 Mechs as Firestorm's pictures call them.
const BT3_PICTURE = { MadCat: "Mad Cat", Vulture: "Vulture", Loki: "Loki", Thor: "Thor", Sunder: "Sunder", Avatar: "Avatar" };
function bt3Picture(id) {
  const v = (B.vehicles || []).find(v => v.id === +id);
  return v && typeof mechImage === "function" ? mechImage(BT3_PICTURE[v.chassis] || v.chassis) : "";
}
function options(list, cur) {
  return list.map(([v, t]) => `<option value="${esc(v)}" ${String(v) === String(cur) ? "selected" : ""}>${esc(t)}</option>`).join("");
}
function vehicleOptions(cur) {
  const groups = {};
  (B.vehicles || []).forEach(v => (groups[v.chassis] = groups[v.chassis] || []).push(v));
  return Object.entries(groups).map(([c, vs]) => `<optgroup label="${esc(c)}">` +
    vs.map(v => `<option value="${v.id}" ${v.id === +cur ? "selected" : ""}>${esc(v.name)}</option>`).join("") + `</optgroup>`).join("");
}

function renderBt3() {
  const s = B3;
  $("#bt3-error").hidden = B.files_ok && B.built;
  $("#bt3-error").textContent = !B.built ? "The emulator is not built: in the battlepod repository, make && make cockpit."
    : !B.files_ok ? "Where is the release? Set its Game Files folder below." : "";
  $("#bt3-checks").innerHTML =
    `<span class="pill ${B.built ? "ok" : "bad"}">${B.built ? "Emulator built" : "Emulator not built"}</span>` +
    `<span class="pill ${B.files_ok ? "ok" : "bad"}">${B.files_ok ? "Release found" : "No release"}</span>` +
    `<span class="pill">${(B.scenarios || []).length} maps, ${(B.vehicles || []).length} 'Mechs</span>`;
  if (document.activeElement !== $("#bt3-files")) $("#bt3-files").value = B.game_files || "";
  $("#bt3-pilot").value = s["game.pilot"] || "";
  $("#bt3-vehicle").innerHTML = vehicleOptions(s["game.vehicle"]);
  $("#bt3-paint").value = s["game.paint"] || "0";
  $("#bt3-accent").value = s["game.accent"] || "0";
  $("#bt3-scenario").innerHTML = options((B.scenarios || []).map(c => [c.file, c.name]), s["game.scenario"]);
  $("#bt3-env").innerHTML = options((B.envs || []).map(e => [e.id, e.name]), s["game.env"]);
  // On a centre's console, the pods in service are the other seats (Pods page).
  const cp = B.centre_pods || [], local = !cp.length || s["game.local"] !== "0";
  $("#bt3-local-row").hidden = !cp.length;
  $("#bt3-local").value = local ? "1" : "0";
  $("#bt3-others-row").hidden = cp.length > 0;
  $("#bt3-start").textContent = local ? "Drop in" : "Start the battle";
  const maxBots = 8 - (local ? 1 : 0) - (cp.length || +s["game.remote"] || 0);
  $("#bt3-bots").innerHTML = options([0, 1, 2, 3, 4, 5, 6, 7].filter(n => n <= maxBots).map(n => [n, n ? `${n} CPU pilot${n > 1 ? "s" : ""}` : "None"]), Math.min(+s["game.bots"] || 0, maxBots));
  $("#bt3-remote").value = s["game.remote"] || "0";
  $("#bt3-join-host").value = s["game.join_host"] || "";
  $("#bt3-join-seat").innerHTML = options([2, 3, 4, 5, 6, 7, 8].map(n => [n, "Seat " + n]), s["game.join_seat"] || 2);
  renderSeats();
  $("#bt3-arena").value = s["game.arena"] === "0" ? "0" : "1";
  $("#bt3-minutes").value = s["game.minutes"] || "0";
  $("#bt3-respawn").value = s["game.respawn"] ?? "10";
  renderOpponents();
  renderPortrait();
  renderRosterPick();
  renderBattle();
  renderBt3Display();
  renderBt3Joy();
}
function renderOpponents() {
  const n = +B3["game.bots"] || 0;
  const opp = (B3["game.opponents"] || "").split(",").filter(x => x !== "");
  while (opp.length < n) opp.push(String([8, 12, 30, 34, 4, 26, 13][opp.length % 7]));
  $("#bt3-opponents").innerHTML = n ? opp.slice(0, n).map((v, i) =>
    `<select data-opp="${i}">${vehicleOptions(v)}</select>`).join("") : `<span class="hint">-</span>`;
  $$("select[data-opp]").forEach(el => el.addEventListener("change", () => {
    const list = (B3["game.opponents"] || "").split(",");
    while (list.length <= +el.dataset.opp) list.push("8");
    list[+el.dataset.opp] = el.value;
    setBt3({ "game.opponents": list.join(",") });
  }));
}
// Seats for people on other PCs: each a name and a Mech the operator gives.
function renderSeats() {
  const cp = B.centre_pods || [];
  const n = cp.length || +B3["game.remote"] || 0, first = cp.length && B3["game.local"] === "0" ? 1 : 2;
  $("#bt3-remote-row").hidden = !n;
  const names = (B3["game.remote_names"] || "").split(","), vs = (B3["game.remote_vehicles"] || "").split(",");
  const DEF = [8, 12, 30, 34, 4, 26, 13];
  $("#bt3-seats").innerHTML = [...Array(n).keys()].map(i =>
    `<span class="seat3"><span class="hint">Seat ${i + first}</span>${cp.length ? `<b>${esc(cp[i])}</b>` :
      `<input data-seatname="${i}" maxlength="8" value="${esc(names[i] || "Guest" + (i + 2))}">`}
     <select data-seatmech="${i}">${vehicleOptions(vs[i] || DEF[i])}</select></span>`).join("");
  const put = (key, i, v, fill) => {
    const list = (B3[key] || "").split(",");
    while (list.length < Math.max(3, i + 1)) list.push(fill(list.length));
    list[i] = v.replace(/,/g, "");
    setBt3({ [key]: list.join(",") });
  };
  $$("input[data-seatname]").forEach(el => el.addEventListener("input", () => put("game.remote_names", +el.dataset.seatname, el.value, k => "Guest" + (k + 2))));
  $$("select[data-seatmech]").forEach(el => el.addEventListener("change", () => put("game.remote_vehicles", +el.dataset.seatmech, el.value, k => String(DEF[k]))));
}
function renderPortrait() {
  const pic = bt3Picture(B3["game.vehicle"]);
  $("#bt3-portrait").style.backgroundImage = pic ? `url('${pic}')` : "";
  $("#bt3-portrait").classList.toggle("none", !pic);
  $("#bt3-tag").textContent = vehicleName(B3["game.vehicle"]);
}
async function renderRosterPick() {
  try {
    if (!P || !P.roster) P = await api("/api/pilots");
  } catch (e) { return; }
  const names = ((P && P.roster) || []).map(p => p.name).sort((a, b) => a.localeCompare(b));
  $("#bt3-roster").innerHTML = `<option value="">From the roster...</option>` + names.map(n => `<option>${esc(n)}</option>`).join("");
  renderPortrait();                     // the pictures come with the roster
}

// Settings are saved as they change (battletech.ini); the next battle uses them.
let bt3SaveTimer = null, bt3Pending = {};
function setBt3(changes, now) {
  Object.assign(B3, changes);
  Object.assign(bt3Pending, changes);   // every change since the last save goes
  clearTimeout(bt3SaveTimer);
  const send = async () => {
    const out = bt3Pending;
    bt3Pending = {};
    const r = await api("/api/bt3/save", out);
    status(r.ok ? "Saved to battletech.ini." : "Error: " + r.error, !r.ok);
  };
  if (now) return send();
  bt3SaveTimer = setTimeout(send, 300);
}
$("#bt3-pilot").addEventListener("input", e => setBt3({ "game.pilot": e.target.value }));
$("#bt3-roster").addEventListener("change", e => {
  if (!e.target.value) return;
  const name = e.target.value.slice(0, 8);
  $("#bt3-pilot").value = name;
  setBt3({ "game.pilot": name });
  e.target.value = "";
});
$("#bt3-vehicle").addEventListener("change", e => { setBt3({ "game.vehicle": e.target.value }); renderPortrait(); });
$("#bt3-paint").addEventListener("change", e => setBt3({ "game.paint": e.target.value }));
$("#bt3-accent").addEventListener("change", e => setBt3({ "game.accent": e.target.value }));
$("#bt3-scenario").addEventListener("change", e => setBt3({ "game.scenario": e.target.value }));
$("#bt3-env").addEventListener("change", e => setBt3({ "game.env": e.target.value }));
$("#bt3-bots").addEventListener("change", e => { setBt3({ "game.bots": e.target.value }); renderOpponents(); });
$("#bt3-arena").addEventListener("change", e => setBt3({ "game.arena": e.target.value }));
$("#bt3-remote").addEventListener("change", e => { setBt3({ "game.remote": e.target.value }); renderBt3(); });
$("#bt3-local").addEventListener("change", e => { setBt3({ "game.local": e.target.value }); renderBt3(); });
$("#bt3-join-host").addEventListener("input", e => setBt3({ "game.join_host": e.target.value.trim() }));
$("#bt3-join-seat").addEventListener("change", e => setBt3({ "game.join_seat": e.target.value }));
$("#bt3-join").addEventListener("click", async () => {
  status("Joining...", true);
  const r = await api("/api/bt3/join", { "game.join_host": $("#bt3-join-host").value.trim(), "game.join_seat": $("#bt3-join-seat").value });
  status(r.ok ? r.message : "Error: " + r.error, !r.ok);
});
$("#bt3-minutes").addEventListener("change", e => setBt3({ "game.minutes": e.target.value }));
$("#bt3-respawn").addEventListener("change", e => setBt3({ "game.respawn": e.target.value }));
$("#bt3-files-save").addEventListener("click", async () => {
  const r = await api("/api/bt3/save", { "game.files": $("#bt3-files").value.trim() });
  status(r.ok ? "The release is " + $("#bt3-files").value.trim() : "Error: " + r.error, !r.ok);
  loadBt3();
});
$("#bt3-start").addEventListener("click", async () => {
  status("Starting the battle...", true);
  const r = await api("/api/bt3/start", B3);
  status(r.ok ? r.message : "Error: " + r.error, !r.ok);
  setTimeout(pollBattle, 1500);
});
$("#bt3-stop").addEventListener("click", async () => {
  const r = await api("/api/bt3/stop", {});
  status(r.ok ? "Battle stopped." : "Error: " + r.error, !r.ok);
  pollBattle();
});

// ------------------------------------------------------------ the operator's view
function renderBattle() {
  const b = B && B.battle;
  const run = B && B.running;
  $("#bt3-start").disabled = !!run || !B.files_ok || !B.built;
  $("#bt3-stop").disabled = !run;
  $("#bt3-start").textContent = run ? "In battle" : "Drop in";
  $("#bt3-clock").textContent = b ? `${b.scenario} - ${Math.floor(b.t / 60)}:${String(Math.floor(b.t % 60)).padStart(2, "0")}${b.over ? " - over" : run ? "" : " - stopped"}` : "No battle yet";
  $("#bt3-log").textContent = (B.log || []).join("\n");
  drawBattle(b);
  const pods = (b && b.pods) || [];
  $("#bt3-score tbody").innerHTML = pods.map(p => {
    const c = BT3_COLOURS[(p.node - 1) % BT3_COLOURS.length];
    const state = p.down ? "destroyed" : p.live ? `${Math.round(p.speed)} kph` : p.joined ? "dropping" : "starting";
    const who = p.remote ? ` <span class="hint">seat ${p.node}${p.joined ? "" : ", waiting"}</span>` : p.human ? ` <span class="hint">you</span>` : "";
    return `<tr><td><span class="dotc" style="background:${c}"></span> ${esc(p.name)}${who}</td>
      <td>${esc(vehicleName(p.vehicle))}</td><td>${state}</td><td class="n">${p.kills}</td><td class="n">${p.deaths}</td><td class="n">${p.hits_taken}</td></tr>`;
  }).join("") || `<tr><td colspan="6" class="hint">Drop in to start one.</td></tr>`;
  const names = Object.fromEntries(pods.map(p => [p.node, p.name]));
  $("#bt3-events").innerHTML = ((b && b.events) || []).slice(-8).reverse().map(e =>
    `<div><span class="hint">${Math.floor(e.t / 60)}:${String(Math.floor(e.t % 60)).padStart(2, "0")}</span> ${esc(names[e.by] || "?")} destroyed ${esc(names[e.thing] || "a Mech")}</div>`).join("");
}
function drawBattle(b) {
  const cv = $("#bt3-map"), g = cv.getContext("2d"), W = cv.width, H = cv.height;
  g.fillStyle = "#050605"; g.fillRect(0, 0, W, H);
  g.strokeStyle = "#1c1f1b"; g.lineWidth = 1;
  for (let i = 1; i < 10; i++) {
    g.beginPath(); g.moveTo(i * W / 10, 0); g.lineTo(i * W / 10, H); g.stroke();
    g.beginPath(); g.moveTo(0, i * H / 10); g.lineTo(W, i * H / 10); g.stroke();
  }
  if (!b || !b.bounds) {
    g.fillStyle = "#6b6e68"; g.font = "16px sans-serif"; g.textAlign = "center";
    g.fillText("The map appears here when a battle starts", W / 2, H / 2);
    return;
  }
  if (b.map) drawBattle.map = b.map;
  const pods = (b.pods || []).filter(p => p.x != null);
  // The whole map, or round the Mechs when they are close together.
  let [x0, y0, x1, y1] = b.bounds;
  if (pods.length) {
    const xs = pods.map(p => p.x), ys = pods.map(p => p.y);
    const cx = (Math.min(...xs) + Math.max(...xs)) / 2, cy = (Math.min(...ys) + Math.max(...ys)) / 2;
    const half = Math.max(250, (Math.max(...xs) - Math.min(...xs)) * 0.75, (Math.max(...ys) - Math.min(...ys)) * 0.75);
    if (half * 2 < Math.max(x1 - x0, y1 - y0)) { x0 = cx - half; x1 = cx + half; y0 = cy - half; y1 = cy + half; }
  }
  const span = Math.max(x1 - x0, y1 - y0) * 1.04, ox = (x0 + x1) / 2 - span / 2, oy = (y0 + y1) / 2 - span / 2;
  const sx = x => (x - ox) / span * W, sy = y => (y - oy) / span * H;
  g.fillStyle = "#3b3f38";
  const r = Math.max(1.2, Math.min(4, 6000 / span));
  (drawBattle.map || []).forEach(([x, y, c]) => {
    const px = sx(x), py = sy(y);
    if (px < -5 || py < -5 || px > W + 5 || py > H + 5) return;
    g.beginPath(); g.arc(px, py, c === 2 ? r * 1.6 : r, 0, 7); g.fill();
  });
  g.font = "bold 13px sans-serif"; g.textAlign = "left";
  pods.forEach(p => {
    const c = BT3_COLOURS[(p.node - 1) % BT3_COLOURS.length], px = sx(p.x), py = sy(p.y);
    g.strokeStyle = c; g.fillStyle = c; g.lineWidth = 3;
    if (p.down) {
      g.beginPath(); g.moveTo(px - 7, py - 7); g.lineTo(px + 7, py + 7); g.moveTo(px - 7, py + 7); g.lineTo(px + 7, py - 7); g.stroke();
    } else {
      const h = (p.heading || 0) * Math.PI / 180;
      g.beginPath(); g.arc(px, py, 7, 0, 7); g.fill();
      g.beginPath(); g.moveTo(px, py); g.lineTo(px + 20 * Math.sin(h), py - 20 * Math.cos(h)); g.stroke();
      if (p.human) { g.lineWidth = 2; g.beginPath(); g.arc(px, py, 12, 0, 7); g.stroke(); }
    }
    g.fillText(p.name, px + 12, py - 10);
  });
  g.fillStyle = "#6b6e68"; g.font = "12px sans-serif";
  g.fillText(`${Math.round(span)} m across`, 10, H - 10);
}
async function pollBattle() {
  if (!$("#page-bt3").classList.contains("on")) return;
  const r = await api("/api/bt3");
  if (!r.available) return;
  const wasRunning = B && B.running;
  B.running = r.running; B.battle = r.battle; B.log = r.log;
  renderBattle();
  if (wasRunning && !r.running) {
    // The result, as the operator would read it out.
    const pods = ((r.battle && r.battle.pods) || []).slice().sort((a, b) => b.kills - a.kills || a.deaths - b.deaths);
    const k = n => `${n} kill${n === 1 ? "" : "s"}`;
    status(pods.length ? "Battle over: " + pods.map(p => `${p.name} ${k(p.kills)}, ${p.deaths} down`).join(" - ")
      + (pods[0].kills ? `. ${pods[0].name} takes it.` : ".") : "The battle is over.", false);
  }
}
setInterval(() => { if (B && B.running) pollBattle(); }, 700);
setInterval(() => { if (B && !B.running) pollBattle(); }, 4000);

// ------------------------------------------------------------ display
// The window keeps the picture's shape: 4:3, and the strip's 84 of 480 under it.
const BT3_WIDTHS = ["960", "1280", "1440", "1920"];
function bt3Height(w, strip) { return Math.round(w * 3 / 4 + (strip ? w * 84 / 480 : 0)); }
function renderBt3Display() {
  const s = B3, w = +(s["view.w"] || 960), h = +(s["view.h"] || 888), strip = s["view.strip"] !== "0";
  const preset = BT3_WIDTHS.includes(String(w)) && h === bt3Height(w, strip);
  $("#bt3d-size").value = preset ? String(w) : "custom";
  $("#bt3d-custom").hidden = preset;
  $("#bt3d-strip").checked = strip;
  $("#bt3d-w").value = s["view.w"] || 960; $("#bt3d-h").value = s["view.h"] || 720;
  $("#bt3d-x").value = s["view.x"] || ""; $("#bt3d-y").value = s["view.y"] || "";
  $("#bt3d-full").checked = s["view.fullscreen"] === "1";
  const mons = (S && S.monitors) || [];
  $("#bt3d-mon").innerHTML = mons.map(m => `<option value="${m.n}">Monitor ${m.n}${m.primary ? " (main)" : ""} - ${m.w} x ${m.h}</option>`).join("");
  $("#bt3d-mon").value = s["view.monitor"] || "1";
  $("#bt3d-smooth").checked = s["view.smooth"] !== "0";
  $("#bt3d-effects").checked = s["view.effects"] !== "0";
  $("#bt3d-panels").checked = s["panels.show"] === "1";
  $("#bt3r-show").checked = s["radar.show"] !== "0";
  const rs = (s["radar.w"] || "960") + "x" + (s["radar.h"] || "600");
  $("#bt3r-size").value = [...$("#bt3r-size").options].some(o => o.value === rs) ? rs : "960x600";
  $("#bt3r-layout").value = s["radar.layout"] || "";
  $("#bt3r-x").value = s["radar.x"] || ""; $("#bt3r-y").value = s["radar.y"] || "";
  $("#bt3r-full").checked = s["radar.fullscreen"] === "1";
  $("#bt3r-mon").innerHTML = $("#bt3d-mon").innerHTML;
  $("#bt3r-mon").value = s["radar.monitor"] || (mons.length > 1 ? "2" : "1");
  $("#bt3r-page").value = s["radar.page"] || "0";
  $("#bt3r-range").value = s["radar.range"] || "1";
}
function bt3RadarFull() {
  const on = $("#bt3r-full").checked, m = ((S && S.monitors) || []).find(m => m.n == $("#bt3r-mon").value);
  if (on && m) setBt3({ "radar.fullscreen": "1", "radar.monitor": String(m.n), "radar.x": String(m.x + 20), "radar.y": String(m.y + 20) });
  else setBt3({ "radar.fullscreen": on ? "1" : "0", "radar.monitor": $("#bt3r-mon").value });
}
$("#bt3r-show").addEventListener("change", e => setBt3({ "radar.show": e.target.checked ? "1" : "0" }));
$("#bt3r-size").addEventListener("change", e => { const [w, h] = e.target.value.split("x"); setBt3({ "radar.w": w, "radar.h": h }); });
$("#bt3r-layout").addEventListener("change", e => setBt3({ "radar.layout": e.target.value }));
["#bt3r-x", "#bt3r-y"].forEach(id => $(id).addEventListener("change", () => setBt3({ "radar.x": $("#bt3r-x").value, "radar.y": $("#bt3r-y").value })));
$("#bt3r-full").addEventListener("change", bt3RadarFull);
$("#bt3r-mon").addEventListener("change", bt3RadarFull);
$("#bt3r-page").addEventListener("change", e => setBt3({ "radar.page": e.target.value }));
$("#bt3r-range").addEventListener("change", e => setBt3({ "radar.range": e.target.value }));
function bt3Fullscreen() {
  // The cockpit goes full screen on the monitor holding [view] x, y: put them on the one chosen.
  const on = $("#bt3d-full").checked, m = ((S && S.monitors) || []).find(m => m.n == $("#bt3d-mon").value);
  if (on && m) setBt3({ "view.fullscreen": "1", "view.monitor": String(m.n), "view.x": String(m.x + 20), "view.y": String(m.y + 20) });
  else setBt3({ "view.fullscreen": on ? "1" : "0", "view.monitor": $("#bt3d-mon").value });
}
$("#bt3d-size").addEventListener("change", e => {
  $("#bt3d-custom").hidden = e.target.value !== "custom";
  if (e.target.value === "custom") return;
  const w = +e.target.value;
  setBt3({ "view.w": String(w), "view.h": String(bt3Height(w, $("#bt3d-strip").checked)) });
});
["#bt3d-w", "#bt3d-h"].forEach(id => $(id).addEventListener("change", () => setBt3({ "view.w": $("#bt3d-w").value, "view.h": $("#bt3d-h").value })));
["#bt3d-x", "#bt3d-y"].forEach(id => $(id).addEventListener("change", () => setBt3({ "view.x": $("#bt3d-x").value, "view.y": $("#bt3d-y").value })));
$("#bt3d-full").addEventListener("change", bt3Fullscreen);
$("#bt3d-mon").addEventListener("change", bt3Fullscreen);
$("#bt3d-smooth").addEventListener("change", e => setBt3({ "view.smooth": e.target.checked ? "1" : "0" }));
$("#bt3d-effects").addEventListener("change", e => setBt3({ "view.effects": e.target.checked ? "1" : "0" }));
$("#bt3d-panels").addEventListener("change", e => setBt3({ "panels.show": e.target.checked ? "1" : "0" }));
$("#bt3d-strip").addEventListener("change", e => {
  const ch = { "view.strip": e.target.checked ? "1" : "0" }, w = +($("#bt3d-w").value || 960);
  if ($("#bt3d-size").value !== "custom") ch["view.h"] = String(bt3Height(w, e.target.checked));
  setBt3(ch);
  renderBt3Display();
});

// ------------------------------------------------------------ USB controllers
const BT3_JOY = [
  ["throttle", "Throttle lever", "axis"], ["stick_x", "Stick, left and right (turn)", "axis"],
  ["fire1", "Fire group 1"], ["fire2", "Fire group 2"], ["fire3", "Fire group 3"], ["fire_all", "Fire all three"],
  ["target", "Target select"], ["light", "Searchlight"], ["twist_left", "Twist torso left"], ["twist_right", "Twist torso right"],
  ["centre", "Centre the torso"], ["view_up", "Look up"], ["view_down", "Look down"], ["zoom_in", "Zoom in"], ["zoom_out", "Zoom out"],
];
let BJ = [], bt3Learn = null;
function joyNow(ref) {
  // What a binding reads now, from /api/joy: "NAME.X" or "NAME.Bn" or "NAME.HatUp".
  if (!ref) return "";
  const neg = ref.startsWith("-"), r = neg ? ref.slice(1) : ref, dot = r.lastIndexOf(".");
  const dev = r.slice(0, dot).toLowerCase(), what = r.slice(dot + 1);
  const d = BJ.find(d => d.ids.toLowerCase() === dev || d.name.toLowerCase().includes(dev) || ("#" + d.n) === dev);
  if (!d) return `<span class="hint">not plugged in</span>`;
  if (/^[XYZRUV]$/i.test(what)) {
    const v = d.axes[what.toUpperCase()];
    if (v == null) return `<span class="hint">no such axis</span>`;
    const x = neg ? 1000 - v : v;
    return `<div class="bar mini"><i style="width:${x / 10}%"></i></div>`;
  }
  const m = /^B(\d+)$/i.exec(what);
  if (m) return d.buttons.includes(+m[1]) ? `<span class="pill ok">held</span>` : "";
  const hat = { hatup: [31500, 4500], hatdown: [13500, 22500], hatleft: [22500, 31500], hatright: [4500, 13500] }[what.toLowerCase()];
  if (hat && d.hat != null) {
    const [a, b] = hat, h = d.hat, on = a > b ? (h >= a || h <= b) : (h >= a && h <= b);
    return on ? `<span class="pill ok">held</span>` : "";
  }
  return "";
}
function renderBt3Joy() {
  if (!B3 || !$("#bt3j-table")) return;
  $("#bt3j-on").checked = B3["joy.enabled"] !== "0";
  $("#bt3j-dead").value = B3["joy.deadzone"] || "4";
  $("#bt3j-table tbody").innerHTML = BT3_JOY.map(([k, label, kind]) => {
    const v = B3["joy." + k] || "", learning = bt3Learn && bt3Learn.key === k;
    return `<tr><td>${esc(label)}</td><td class="mono">${v ? esc(v) : `<span class="hint">-</span>`}</td><td class="joynow" data-now="${k}">${joyNow(v)}</td>
      <td><button class="btn ghost" data-bt3learn="${k}">${learning ? (kind === "axis" ? "Move it..." : "Press it...") : "Learn"}</button>
      <button class="btn ghost" data-bt3clear="${k}" ${v ? "" : "disabled"}>Clear</button></td></tr>`;
  }).join("");
  $$("button[data-bt3learn]").forEach(el => el.addEventListener("click", () => {
    const k = el.dataset.bt3learn, kind = (BT3_JOY.find(j => j[0] === k) || [])[2];
    bt3Learn = { key: k, axis: kind === "axis", base: JSON.parse(JSON.stringify(BJ)), until: Date.now() + 8000 };
    renderBt3Joy();
  }));
  $$("button[data-bt3clear]").forEach(el => el.addEventListener("click", () => {
    setBt3({ ["joy." + el.dataset.bt3clear]: "none" }); renderBt3Joy();
  }));
  $("#bt3j-devs").innerHTML = BJ.length ? "Plugged in: " + BJ.map(d => `<b>${esc(d.name)}</b> (${d.ids}, ${Object.keys(d.axes).length} axes, ${d.nbuttons} buttons${d.has_hat ? ", a hat" : ""})`).join("; ")
    : "No USB controllers plugged in.";
}
function bt3LearnStep() {
  if (!bt3Learn) return;
  if (Date.now() > bt3Learn.until) { bt3Learn = null; status("Nothing moved: not learned.", true); renderBt3Joy(); return; }
  const name = d => d.name.startsWith("controller ") ? d.ids : d.name;
  for (const d of BJ) {
    const was = bt3Learn.base.find(b => b.n === d.n);
    if (!was) continue;
    if (bt3Learn.axis) {
      for (const [a, v] of Object.entries(d.axes)) {
        if (Math.abs(v - (was.axes[a] ?? v)) > 350) {
          // A lever moved toward its high end is the right way round; toward the low end, reversed.
          const ref = (v < was.axes[a] && bt3Learn.key === "throttle" ? "-" : "") + `${name(d)}.${a}`;
          setBt3({ ["joy." + bt3Learn.key]: ref }); bt3Learn = null; renderBt3Joy(); return;
        }
      }
    } else {
      const b = d.buttons.find(b => !was.buttons.includes(b));
      if (b) { setBt3({ ["joy." + bt3Learn.key]: `${name(d)}.B${b}` }); bt3Learn = null; renderBt3Joy(); return; }
      if (d.hat != null && was.hat == null) {
        const h = d.hat, dir = h >= 31500 || h <= 4500 ? "HatUp" : h <= 13500 ? "HatRight" : h <= 22500 ? "HatDown" : "HatLeft";
        setBt3({ ["joy." + bt3Learn.key]: `${name(d)}.${dir}` }); bt3Learn = null; renderBt3Joy(); return;
      }
    }
  }
}
$("#bt3j-on").addEventListener("change", e => setBt3({ "joy.enabled": e.target.checked ? "1" : "0" }));
$("#bt3j-dead").addEventListener("change", e => setBt3({ "joy.deadzone": e.target.value }));
setInterval(async () => {
  if (!$("#page-bt3controls").classList.contains("on")) return;
  const r = await api("/api/joy");
  BJ = r.joysticks || [];
  bt3LearnStep();
  if (bt3Learn) return renderBt3Joy();
  $$("[data-now]").forEach(el => { el.innerHTML = joyNow(B3["joy." + el.dataset.now] || ""); });
  if (!$("#bt3j-devs").dataset.n || +$("#bt3j-devs").dataset.n !== BJ.length) { $("#bt3j-devs").dataset.n = BJ.length; renderBt3Joy(); }
}, 150);

// The pages ask for their state when shown.
window.addEventListener("hashchange", () => {
  const p = location.hash.slice(1);
  if (p.startsWith("bt3")) loadBt3();
  if (gameOf(p)) setGame(gameOf(p), true);
});
if (location.hash.slice(1).startsWith("bt3")) loadBt3();
