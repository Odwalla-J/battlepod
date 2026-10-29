// Firestorm launcher: the page. Reads /api/state, edits a copy of its
// settings, posts them to /api/save; starts and stops through /api/start.
"use strict";
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
let S = null;          // the state as loaded
let V = null;          // the settings being edited
let dirty = false;

const api = (path, body) => fetch(path, body === undefined ? {} : {
  method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
}).then(r => r.json());

function status(text, isDirty, isError) {
  const el = $("#status");
  el.textContent = text;
  el.classList.toggle("dirty", !!isDirty);
  el.classList.toggle("err", !!isError || /^Error/.test(text));
}
window.addEventListener("error", e => status("Launcher error: " + e.message, true, true));
window.addEventListener("unhandledrejection", e => status("Launcher error: " + (e.reason && e.reason.message || e.reason), true, true));
function touch() { dirty = true; status("Unsaved changes", true); drawMap(); }

// ------------------------------------------------------------ navigation
function show(page) {
  $$(".nav").forEach(a => a.classList.toggle("on", a.dataset.page === page));
  $$(".page").forEach(p => p.classList.toggle("on", p.id === "page-" + page));
  if (page === "launch") refreshRun();
  if (page === "pilots") loadPilots();
}
window.addEventListener("hashchange", () => show(location.hash.slice(1) || "pc"));

// ------------------------------------------------------------ simple fields
const TEXT = ["mode", "bitdepth", "main_monitor", "main_x", "main_y", "main_w", "main_h", "mfd_layout", "mfd_tint",
  "wall_cols", "radar_rotate", "pad_index", "pad_deadzone", "ramp_ms", "capture_mouse", "vsync", "max_fps",
  "print_to_file", "print_folder", "console_addr"];
const CHECK = ["keep_aspect", "show_surface", "plasma", "pad_enabled", "pad_invert_y", "gpu_present", "frame_stats",
  "start_printer"];

function fillFields() {
  const opts = (sel, items) => { $(sel).innerHTML = items.map(([v, t]) => `<option value="${v}">${t}</option>`).join(""); };
  opts("#mode", S.modes.map(m => [m, m.replace("x", " x ")]));
  const mons = S.monitors.map(m => [m.n, `Monitor ${m.n}${m.primary ? " (main)" : ""} - ${m.w} x ${m.h}`]);
  opts("#main_monitor", mons);
  opts("#arrange_mon", mons);
  if (S.monitors.length > 1) $("#arrange_mon").value = S.monitors[1].n;

  for (const id of TEXT) { const el = $("#" + id); if (el) el.value = V[id] ?? ""; }
  for (const id of CHECK) $("#" + id).checked = !!V[id];
  $("#main_fullscreen").value = V.main_fullscreen ? "1" : "0";
  $$("input[name=role]").forEach(r => r.checked = r.value === V.role);
  labels();
}
function labels() {
  $("#pad_deadzone_v").textContent = Math.round(V.pad_deadzone / 327.67) + "%";
  $("#ramp_ms_v").textContent = V.ramp_ms == 0 ? "off: full at once" : (V.ramp_ms / 1000).toFixed(2) + " s";
  const full = V.main_fullscreen;
  $$(".only-full").forEach(e => e.style.display = full ? "" : "none");
  $$(".only-win").forEach(e => e.style.display = full ? "none" : "");
  $$(".only-wall").forEach(e => e.style.display = V.mfd_layout === "wall" ? "" : "none");
  const seen = S.pads.length ? `Connected: ${S.pads.map(i => i + 1).join(", ")}` : "No pad connected";
  $("#pad_seen").textContent = seen;
  const lead = {
    pod: "This PC is a pod. Start it, and it waits for the console to send it a match.",
    console: "This PC is the console. Start it, then set up and launch matches on its screen.",
    both: "This PC runs a pod and the console together.",
  }[V.role];
  $("#launch-lead").textContent = lead;
  $("#go").textContent = { pod: "Start the pod", console: "Start the console", both: "Start pod and console" }[V.role];
  $$(".only-pod").forEach(e => e.style.display = V.role === "pod" ? "" : "none");
  $$(".only-console").forEach(e => e.style.display = V.role === "pod" ? "none" : "");
}
function wireFields() {
  for (const id of TEXT) {
    const el = $("#" + id); if (!el) continue;
    el.addEventListener("input", () => { V[id] = el.value; labels(); renderWindows(); touch(); });
  }
  for (const id of CHECK) $("#" + id).addEventListener("change", e => { V[id] = e.target.checked; touch(); });
  $("#main_fullscreen").addEventListener("change", e => { V.main_fullscreen = e.target.value === "1"; labels(); touch(); });
  $$("input[name=role]").forEach(r => r.addEventListener("change", () => { V.role = r.value; labels(); touch(); }));
}

// ------------------------------------------------------------ This PC
function renderPC() {
  $("#gamepath").textContent = S.game;
  $("#pc-game").textContent = S.game;
  $("#pc-shim").innerHTML = (S.found ? '<span class="pill ok">MW4.exe found</span>' : '<span class="pill bad">no MW4.exe here</span>') +
    (S.shim_installed ? '<span class="pill ok">shim installed</span>' : '<span class="pill bad">shim not installed</span>');
  $("#pc-net").innerHTML = S.addresses.map(a => `<span class="pill">${a}</span>`).join("") || '<span class="hint">no address</span>';
  $("#pc-mons").innerHTML = S.monitors.map(m => `<div>${m.n}. ${m.w} x ${m.h}${m.primary ? " - main" : ""} <span class="hint">at ${m.x}, ${m.y}</span></div>`).join("");
  $("#pc-pads").innerHTML = S.pads.length ? S.pads.map(i => `<span class="pill ok">Pad ${i + 1}</span>`).join("") : '<span class="hint">None connected</span>';
}

// ------------------------------------------------------------ windows and the map
function visibleWindows() {
  return S.windows_meta.filter(w => V.mfd_layout === "wall" ? !/^mfd\d$/.test(w.sec) : w.sec !== "mfd_wall")
    .filter(w => w.sec !== "plasma" || V.plasma);
}
function renderWindows() {
  const tb = $("#wintable tbody");
  tb.innerHTML = visibleWindows().map(w => {
    const v = V.windows[w.sec] || {};
    const f = k => `<input data-sec="${w.sec}" data-k="${k}" value="${v[k] ?? ""}" placeholder="auto">`;
    return `<tr><td>${w.title}</td><td>${f("x")}</td><td>${f("y")}</td><td>${f("w")}</td><td>${f("h")}</td>
      <td><input type="checkbox" data-sec="${w.sec}" data-k="borderless" ${v.borderless === "1" || v.borderless === true ? "checked" : ""}></td></tr>`;
  }).join("");
  $$("input", tb).forEach(el => el.addEventListener(el.type === "checkbox" ? "change" : "input", () => {
    const w = V.windows[el.dataset.sec] = V.windows[el.dataset.sec] || {};
    w[el.dataset.k] = el.type === "checkbox" ? (el.checked ? "1" : "") : el.value;
    touch();
  }));
}
function mainRect() {
  const [gw, gh] = V.mode.split("x").map(Number);
  if (V.main_fullscreen) {
    const m = S.monitors.find(m => m.n == V.main_monitor) || S.monitors[0];
    return { x: m.x, y: m.y, w: m.w, h: m.h };
  }
  return { x: +V.main_x || 0, y: +V.main_y || 0, w: +V.main_w || gw, h: +V.main_h || gh };
}
function drawMap() {
  const svg = $("#map");
  if (!S) return;
  const xs = S.monitors.flatMap(m => [m.x, m.x + m.w]), ys = S.monitors.flatMap(m => [m.y, m.y + m.h]);
  const x0 = Math.min(...xs), y0 = Math.min(...ys), x1 = Math.max(...xs), y1 = Math.max(...ys);
  const pad = 40;
  svg.setAttribute("viewBox", `${x0 - pad} ${y0 - pad} ${x1 - x0 + 2 * pad} ${y1 - y0 + 2 * pad}`);
  // Labels a fixed size on screen, whatever the desktop's size.
  const vw = x1 - x0 + 2 * pad, vh = y1 - y0 + 2 * pad;
  const scale = Math.min((svg.clientWidth || 900) / vw, (svg.clientHeight || 300) / vh);
  const k = 0.85 / scale;
  let out = S.monitors.map(m => `<rect class="mon" x="${m.x}" y="${m.y}" width="${m.w}" height="${m.h}" rx="${12 * k}"/>
    <text class="monlabel" x="${m.x + 14 * k}" y="${m.y + m.h - 14 * k}" style="font-size:${13 * k}px">Monitor ${m.n} - ${m.w} x ${m.h}</text>`).join("");
  const r = mainRect();
  out += `<rect class="win main" x="${r.x}" y="${r.y}" width="${r.w}" height="${r.h}"/>
    <text class="winlabel" x="${r.x + 8 * k}" y="${r.y + 18 * k}" style="font-size:${13 * k}px">Main view</text>`;
  // Where the shim puts a window it is given no place for: the second
  // monitor's work area (else the primary's right-hand side), at these offsets.
  const second = S.monitors.find(m => !m.primary);
  const area = second ? second.work : (p => [p.work[0] + Math.max(0, p.work[2] - 1000), p.work[1]])(S.monitors[0]);
  const wall = V.mfd_layout === "wall", turned = V.radar_rotate === "90" || V.radar_rotate === "270";
  const DEF = { mfd1: [0, 0], mfd2: [330, 0], mfd3: [660, 0], mfd4: [0, 280], mfd5: [330, 280],
    radar: wall ? [0, 290] : [990, 0], plasma: [1500, 0], mfd_wall: [0, 0] };
  const SIZE = { radar: turned ? [480, 640] : [640, 480] };
  const set = v => v !== undefined && v !== null && v !== "";
  for (const w of visibleWindows()) {
    const v = V.windows[w.sec] || {};
    const d = DEF[w.sec] || [0, 0], sz = SIZE[w.sec] || [w.w, w.h];
    const placed = set(v.x) && set(v.y);
    const x = placed ? +v.x : area[0] + d[0], y = placed ? +v.y : area[1] + d[1];
    const ww = set(v.w) ? +v.w : sz[0], hh = set(v.h) ? +v.h : sz[1];
    out += `<rect class="win${placed ? "" : " auto"}" x="${x}" y="${y}" width="${ww}" height="${hh}"/>
      <text class="winlabel" x="${x + 6 * k}" y="${y + 16 * k}" style="font-size:${12 * k}px">${w.title}</text>`;
  }
  svg.innerHTML = out;
}
function arrange() {
  const mon = S.monitors.find(m => m.n == $("#arrange_mon").value);
  if (!mon) return;
  // The part the taskbar leaves free (on a pod's own monitors, all of it).
  const m = { x: mon.work[0], y: mon.work[1], w: mon.work[2], h: mon.work[3] };
  const nb = $("#arrange_borderless").checked ? "1" : "";
  const W = V.windows;
  // Top row: the five MFDs (or the wall), 4:3.
  let cw = Math.floor(m.w / 5), ch = Math.floor(cw * 3 / 4);
  if (ch > m.h / 2) { ch = Math.floor(m.h / 2); cw = Math.floor(ch * 4 / 3); }
  if (V.mfd_layout === "wall") {
    W.mfd_wall = { x: m.x, y: m.y, w: cw * 5, h: ch, borderless: nb };
    V.wall_cols = "5";
  } else {
    for (let i = 0; i < 5; i++) W["mfd" + (i + 1)] = { x: m.x + i * cw, y: m.y, w: cw, h: ch, borderless: nb };
  }
  // Below: the radar (upright if turned), then the plasma sign beside it.
  const rh = m.h - ch;
  let rw, rhh;
  if (V.radar_rotate === "90" || V.radar_rotate === "270") { rhh = rh; rw = Math.floor(rh * 3 / 4); }
  else { rhh = rh; rw = Math.floor(rh * 4 / 3); if (rw > m.w / 2) { rw = Math.floor(m.w / 2); rhh = Math.floor(rw * 3 / 4); } }
  W.radar = { x: m.x, y: m.y + ch, w: rw, h: rhh, borderless: nb };
  const pw = Math.min(m.w - rw, 1344), ph = Math.floor(pw * 192 / 672);
  W.plasma = { x: m.x + rw, y: m.y + ch, w: pw, h: ph, borderless: nb };
  V.show_surface = false; $("#show_surface").checked = false;
  renderWindows(); touch();
}

// ------------------------------------------------------------ bindings
const CODES = { Space: "Space", Enter: "Enter", Tab: "Tab", Backslash: "Backslash", ShiftLeft: "Shift", ShiftRight: "Shift",
  ControlLeft: "Ctrl", ControlRight: "Ctrl", AltLeft: "Alt", AltRight: "Alt", ArrowUp: "Up", ArrowDown: "Down",
  ArrowLeft: "Left", ArrowRight: "Right", PageUp: "PageUp", PageDown: "PageDown", Home: "Home", End: "End",
  Insert: "Insert", Delete: "Delete", ScrollLock: "ScrollLock", Backspace: "Backspace", Minus: "Minus", Equal: "Equals",
  Comma: "Comma", Period: "Period", Slash: "Slash", Semicolon: "Semicolon", Quote: "Quote",
  BracketLeft: "LBracket", BracketRight: "RBracket" };
function keyName(e) {
  if (/^Key[A-Z]$/.test(e.code)) return e.code.slice(3);
  if (/^Digit\d$/.test(e.code)) return e.code.slice(5);
  if (/^F\d{1,2}$/.test(e.code)) return e.code;
  if (/^Numpad\d$/.test(e.code)) return "Num" + e.code.slice(6);
  return CODES[e.code] || null;
}
let listening = null;
function renderBinds() {
  const tb = $("#bindtable tbody");
  tb.innerHTML = V.binds.map((b, i) => {
    const keys = b.keys === "none" ? [] : b.keys.split(/[ ,]+/).filter(Boolean);
    const chips = keys.map((k, j) => `<span class="key" data-i="${i}" data-j="${j}" title="Remove">${k}</span>`).join("");
    const add = keys.length < 4 ? `<span class="key add${listening === i ? " listening" : ""}" data-add="${i}">${listening === i ? "press a key" : "+"}</span>` : "";
    const opts = S.pad_buttons.map(p => `<option ${p === b.pad ? "selected" : ""}>${p}</option>`).join("");
    return `<tr><td>${b.label}</td><td><div class="keys">${chips}${add}</div></td><td><select data-pad="${i}">${opts}</select></td></tr>`;
  }).join("");
  $$(".key[data-j]", tb).forEach(el => el.addEventListener("click", () => {
    const b = V.binds[el.dataset.i];
    const keys = b.keys.split(/[ ,]+/).filter(k => k && k !== "none");
    keys.splice(+el.dataset.j, 1);
    b.keys = keys.length ? keys.join(" ") : "none";
    renderBinds(); touch();
  }));
  $$(".key[data-add]", tb).forEach(el => el.addEventListener("click", () => { listening = +el.dataset.add; renderBinds(); }));
  $$("select[data-pad]", tb).forEach(el => el.addEventListener("change", () => { V.binds[el.dataset.pad].pad = el.value; touch(); }));
}
document.addEventListener("keydown", e => {
  if (listening === null) return;
  e.preventDefault();
  if (e.code !== "Escape") {
    const name = keyName(e);
    const b = V.binds[listening];
    const keys = b.keys.split(/[ ,]+/).filter(k => k && k !== "none");
    if (name && !keys.includes(name)) { keys.push(name); b.keys = keys.join(" "); touch(); }
    else if (!name) status(`That key (${e.code}) cannot be bound`, true);
  }
  listening = null;
  renderBinds();
});

// ------------------------------------------------------------ console pods
function renderTeslas() {
  const tb = $("#teslatable tbody");
  tb.innerHTML = V.teslas.map((t, i) => `<tr>
    <td><input type="checkbox" data-i="${i}" data-k="enabled" ${t.enabled ? "checked" : ""}></td>
    <td><input data-i="${i}" data-k="ip" value="${t.ip}"></td>
    <td><input data-i="${i}" data-k="name" value="${t.name}"></td>
    <td><button class="btn ghost" data-del="${i}">Remove</button></td></tr>`).join("") ||
    `<tr><td colspan="4" class="hint">No pods yet.</td></tr>`;
  $$("input", tb).forEach(el => el.addEventListener(el.type === "checkbox" ? "change" : "input", () => {
    V.teslas[el.dataset.i][el.dataset.k] = el.type === "checkbox" ? el.checked : el.value; touch();
  }));
  $$("[data-del]", tb).forEach(el => el.addEventListener("click", () => { V.teslas.splice(+el.dataset.del, 1); renderTeslas(); touch(); }));
}

// ------------------------------------------------------------ running
async function refreshRun() {
  const r = await api("/api/processes");
  S.pads = r.pads;
  $("#procs").innerHTML = r.processes.length ? r.processes.map(p => `<div class="proc"><span>${p.role}</span><span class="hint">${p.name} - ${p.pid}</span></div>`).join("")
    : '<span class="hint">Nothing running.</span>';
  if (r.message) status(r.message, dirty);
  const logs = await api("/api/logs");
  const lines = logs[$("#logrole").value] || ["(no log yet)"];
  const pre = $("#log");
  pre.textContent = lines.map(l => l.replace(/^\d+\s+/, "")).join("\n");
  pre.scrollTop = pre.scrollHeight;
}
async function start(what, extra = {}) {
  if (dirty) await save();
  const r = await api("/api/start", { what, ...extra });
  status(r.message || r.error || "", false);
  setTimeout(refreshRun, 1500);
}

// ------------------------------------------------------------ load and save
async function load() {
  S = await api("/api/state");
  V = JSON.parse(JSON.stringify(S.settings));
  V.role = S.role;
  V.main_fullscreen = !!V.main_fullscreen;
  fillFields(); renderPC(); renderWindows(); renderBinds(); renderTeslas(); drawMap();
  if (S.mechs && !$("#ed-name").value) edit(null);
  dirty = false;
  status(S.found ? "Ready." : "No MW4.exe in the game folder - start the launcher with --game.", !S.found);
}
async function save() {
  const r = await api("/api/save", V);
  dirty = !r.ok;
  status(r.ok ? "Saved to shim.ini, options.ini and ctcl.ini (the old ones are kept as .bak)." : "Error: " + r.error, !r.ok);
}

$("#save").addEventListener("click", save);
$("#revert").addEventListener("click", load);
$("#arrange").addEventListener("click", arrange);
$("#binds_reset").addEventListener("click", () => {
  V.binds.forEach(b => { b.keys = b.default_keys; b.pad = b.default_pad; }); renderBinds(); touch();
});
$("#tesla_add").addEventListener("click", () => {
  V.teslas.push({ ip: S.addresses[0] || "", name: "Pilot" + (V.teslas.length + 1), enabled: true, mark: "" });
  renderTeslas(); touch();
});
$("#tesla_fill").addEventListener("click", () => {
  const base = prompt("First pod's address (the rest count up from it):", "200.0.0.1");
  if (!base) return;
  const p = base.split("."), last = +p.pop();
  V.teslas = Array.from({ length: 16 }, (_, i) => ({ ip: [...p, last + i].join("."), name: "Pilot" + (i + 1), enabled: true, mark: "" }));
  renderTeslas(); touch();
});
$("#go").addEventListener("click", () => start(V.role));
$("#stop").addEventListener("click", () => start("stop"));
$("#test").addEventListener("click", () => start("test", { pilot: $("#test_pilot").value }));
$("#logrole").addEventListener("change", refreshRun);
window.addEventListener("beforeunload", e => { if (dirty) { e.preventDefault(); e.returnValue = ""; } });

// Keep the server alive while the window is open; refresh the launch page.
setInterval(() => fetch("/api/ping").catch(() => {}), 2000);
setInterval(() => { if ($("#page-launch").classList.contains("on")) refreshRun(); }, 4000);

wirePilots();
wireFields();
load().then(() => show(location.hash.slice(1) || "pc"));

// ------------------------------------------------------------ pilots
let P = null, selected = null;
const esc = t => String(t ?? "").replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
function mechImage(chassis) {
  const m = (P && P.mechs) || (S && S.mechs) || {};
  const f = (m.images || {})[chassis];
  return f ? "/game/" + f.split("/").map(encodeURIComponent).join("/") : "";
}
function portrait() {
  const c = $("#ed-chassis").value, v = $("#ed-variant");
  const img = mechImage(c), el = $("#ed-portrait");
  el.classList.toggle("none", !img);
  el.style.backgroundImage = img ? `url("${img}")` : "";
  $("#ed-tag").textContent = c + (v.value ? " " + v.options[v.selectedIndex].text : "");
  renderSpec(v.value || c);
}

// ---- what a 'Mech carries, as the game showed it (tools/mechcat.py)
function weaponLabel(n) {
  // OCR's I and O for 1 and 0 after a weapon type; "REAR" for a weapon facing back
  let s = n.replace(/(AC|LRM|SRM|MRM|ATM|SSRM|LBX|LB)([0-9IOLS]+)(?=REAR|$)/,
    (m, t, d) => t + d.replace(/[IL]/g, "1").replace(/O/g, "0").replace(/S/g, "5"));
  const rear = s.endsWith("REAR");
  if (rear) s = s.slice(0, -4);
  let clan = false;
  if (s.startsWith("CLAN")) { clan = true; s = s.slice(4); }
  const named = { MACHINEGUN: "Machine Gun", GAUSSRIFLE: "Gauss Rifle", LIGHTGAUSSRIFLE: "Light Gauss Rifle", FLAMER: "Flamer",
    PPC: "PPC", ERPPC: "ER PPC", NARC: "NARC Beacon", NARCBEACON: "NARC Beacon", TAG: "TAG", AMS: "Anti-Missile System", LONGTOM: "Long Tom",
    HEAVYGAUSSRIFLE: "Heavy Gauss Rifle", THUNDERBOLT: "Thunderbolt" };
  if (named[s]) s = named[s];
  else {
    s = s.replace(/^ULTRAAC(\d+)$/, "Ultra AC/$1").replace(/^LBXAC(\d+)$/, "LB $1-X AC").replace(/^RAC(\d+)$/, "Rotary AC/$1")
      .replace(/^AC(\d+)$/, "AC/$1").replace(/^SSRM(\d+)$/, "Streak SRM $1").replace(/^STREAKSRM(\d+)$/, "Streak SRM $1")
      .replace(/^(LRM|SRM|MRM|ATM)(\d+)$/, "$1 $2").replace(/^ER/, "ER ").replace(/^HEAVY/, "Heavy ")
      .replace(/(LARGE|MEDIUM|SMALL)?(PULSE)?(LASER)$/, (m, size, pulse, l) =>
        [size && size[0] + size.slice(1).toLowerCase(), pulse && "Pulse", "Laser"].filter(Boolean).join(" "));
  }
  return (clan ? "Clan " : "") + s + (rear ? " (rear)" : "");
}
function renderSpec(key) {
  const m = (P && P.mechs) || (S && S.mechs) || {};
  const sp = (m.specs || {})[key], el = $("#ed-spec");
  if (!sp) {
    el.innerHTML = `<div class="hint">No loadout read for this 'Mech yet (tools/mechcat.py).</div>`;
    return;
  }
  const st = sp.stats || {};
  const bar = (label, v) => v == null ? "" :
    `<div class="bar-row"><span>${label}</span><div class="meter"><i style="width:${Math.max(0, Math.min(100, v))}%"></i></div><b>${v}</b></div>`;
  const groups = [];
  for (const w of sp.weapons || []) {
    const g = groups.find(g => g.name === w.name);
    if (g) { g.n++; g.ammo = g.ammo != null || w.ammo != null ? (g.ammo || 0) + (w.ammo || 0) : null; }
    else groups.push({ name: w.name, n: 1, ammo: w.ammo });
  }
  el.innerHTML = `
    <div class="spec-head"><b>${st.tonnage != null ? st.tonnage + " t" : ""}</b> ${esc(st.class || "")} <span class="hint">${esc(st.tech || "")}</span></div>
    ${bar("Firepower", st.firepower)}${bar("Armor", st.armor)}${bar("Speed", st.speed)}${bar("Heat eff.", st.heat_efficiency)}
    <div class="weapons">${groups.map(g => `<div><span class="n">${g.n}x</span> ${esc(weaponLabel(g.name))}${g.ammo != null ? ` <span class="hint">${g.ammo} rounds</span>` : ""}</div>`).join("")
      || `<div class="hint">No weapons read.</div>`}</div>
    <div class="hint spec-foot">Accel ${st.accel ?? "?"} / decel ${st.decel ?? "?"} m/s, turn ${st.turn_rate ?? "?"}&deg;/s, torso ${st.torso_range ?? "?"}&deg; at ${st.twist_speed ?? "?"}&deg;/s</div>`;
}
function mechLabel(p) {
  if (!p.variant) return p.chassis + " (stock)";
  const v = (P.mechs.variants[p.chassis] || []).find(v => v.file === p.variant);
  return p.chassis + " " + (v ? v.label : p.variant);
}
function seatOf(id) {
  return Object.entries(P.seats || {}).find(([ip, s]) => s.profile === id);
}
function podName(ip) {
  const t = (P.pods || []).find(t => t.ip === ip);
  return t ? `${t.slot} (${ip})` : ip;
}
async function loadPilots() {
  P = await api("/api/pilots");
  const err = $("#pilots-error");
  err.hidden = !P.error;
  if (P.error) {
    err.textContent = P.error;
    $("#roster").innerHTML = "";
    $("#pilots-lead").textContent = "Pilots live on the console; this pod reaches them through the console's launcher.";
    return;
  }
  $("#pilots-lead").textContent = P.local
    ? "Everyone who flies here. Seat a pilot in a pod; at LOAD the console gives that pod their name and 'Mech."
    : `The console's roster (${P.console}). Pick yourself, or make a new pilot, then sit in this pod.`;
  fillChassis();
  renderRoster();
  renderSeats();
  const cur = selected && P.roster.find(p => p.id === selected);
  if (cur) edit(cur); else if (!$("#ed-name").value) edit(null);
}
function renderRoster() {
  const f = ($("#pilot_filter").value || "").toLowerCase();
  const list = P.roster.filter(p => !f || p.name.toLowerCase().includes(f)).sort((a, b) => a.name.localeCompare(b.name));
  $("#roster").innerHTML = list.map(p => {
    const seat = seatOf(p.id);
    const here = seat && !P.local && seat[0] === P.you;
    const action = P.local ? "" : `<button class="btn ${here ? "ghost" : "primary"}" data-sit="${p.id}">${here ? "Leave" : "Sit here"}</button>`;
    return `<div class="pilot${p.id === selected ? " sel" : ""}" data-id="${p.id}">
      <div class="avatar${mechImage(p.chassis) ? "" : " none"}" style="${mechImage(p.chassis) ? `background-image:url('${mechImage(p.chassis)}')` : ""}"></div>
      <div class="who"><div class="nm">${esc(p.name)}</div><div class="mc">${esc(mechLabel(p))}</div></div>
      ${seat ? `<div class="seat-tag">${here ? "in this pod" : "in " + esc(podName(seat[0]))}</div>` : ""}
      ${action}</div>`;
  }).join("") || `<div class="empty">No pilots yet - make the first on the right.</div>`;
  $$(".pilot", $("#roster")).forEach(el => el.addEventListener("click", e => {
    if (e.target.dataset.sit) return;
    selected = el.dataset.id; edit(P.roster.find(p => p.id === selected)); renderRoster();
  }));
  $$("[data-sit]", $("#roster")).forEach(el => el.addEventListener("click", async () => {
    const seat = seatOf(el.dataset.sit);
    const leaving = seat && seat[0] === P.you;
    const r = await api("/api/seat", { id: leaving ? null : el.dataset.sit });
    status(r.ok ? (leaving ? "Left this pod." : `${r.profile.name} is in this pod for the next match.`) : "Error: " + r.error, !r.ok);
    loadPilots();
  }));
}
function renderSeats() {
  if (!P.local) return;
  const opts = id => `<option value="">- empty -</option>` +
    P.roster.slice().sort((a, b) => a.name.localeCompare(b.name)).map(p => `<option value="${p.id}" ${p.id === id ? "selected" : ""}>${esc(p.name)} - ${esc(mechLabel(p))}</option>`).join("");
  $("#seats").innerHTML = (P.pods || []).map(t => {
    const s = (P.seats || {})[t.ip];
    return `<div class="seat"><div class="dot${s ? "" : " off"}"></div><div class="pod"><b>${esc(t.slot)}</b><div class="hint">${esc(t.ip)}</div></div>
      <select data-pod="${esc(t.ip)}">${opts(s && s.profile)}</select></div>`;
  }).join("") || `<div class="empty">No pods in the console's list (Console page).</div>`;
  $$("select[data-pod]", $("#seats")).forEach(el => el.addEventListener("change", async () => {
    const r = await api("/api/seat", { pod: el.dataset.pod, id: el.value || null });
    status(r.ok ? (r.profile ? `${r.profile.name} seated in ${podName(el.dataset.pod)}.` : `${podName(el.dataset.pod)} emptied.`) : "Error: " + r.error, !r.ok);
    loadPilots();
  }));
}
function fillChassis(sel) {
  const c = $("#ed-chassis");
  const m = (P && P.mechs) || S.mechs;
  if (!c.options.length) c.innerHTML = m.chassis.map(n => `<option>${esc(n)}</option>`).join("");
  if (sel) c.value = sel;
  fillVariants();
}
function fillVariants(sel) {
  const m = (P && P.mechs) || S.mechs;
  const vs = m.variants[$("#ed-chassis").value] || [];
  $("#ed-variant").innerHTML = `<option value="">Stock</option>` + vs.map(v => `<option value="${esc(v.file)}">${esc(v.label)}</option>`).join("");
  if (sel !== undefined) $("#ed-variant").value = sel;
  portrait();
}
function edit(p) {
  selected = p ? p.id : null;
  $("#ed-title").textContent = p ? "Edit " + p.name : "New pilot";
  $("#ed-name").value = p ? p.name : "";
  fillChassis(p ? p.chassis : "Loki");
  fillVariants(p ? p.variant : "");
  $("#ed-notes").value = p ? p.notes || "" : "";
  $("#ed-delete").disabled = !p;
}
function wirePilots() {
  $("#pilot_filter").addEventListener("input", () => P && renderRoster());
  $("#ed-chassis").addEventListener("change", () => fillVariants(""));
  $("#ed-variant").addEventListener("change", portrait);
  $("#ed-new").addEventListener("click", () => { edit(null); if (P) renderRoster(); $("#ed-name").focus(); });
  $("#ed-save").addEventListener("click", async () => {
    const body = { id: selected, name: $("#ed-name").value, chassis: $("#ed-chassis").value, variant: $("#ed-variant").value, notes: $("#ed-notes").value };
    const r = await api("/api/pilot", body);
    if (!r.ok) return status("Error: " + r.error, true);
    selected = r.profile.id;
    status(`Saved ${r.profile.name}.`, false);
    loadPilots();
  });
  $("#ed-delete").addEventListener("click", async () => {
    if (!selected || !confirm("Remove this pilot from the roster?")) return;
    const r = await api("/api/pilot/delete", { id: selected });
    status(r.ok ? "Removed." : "Error: " + r.error, !r.ok);
    selected = null; edit(null); loadPilots();
  });
}
