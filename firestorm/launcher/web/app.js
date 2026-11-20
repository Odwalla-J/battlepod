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
  if (page === "mfds") loadMfd();
  if (page === "leaderboards") loadBoards();
}
window.addEventListener("hashchange", () => show(location.hash.slice(1) || "pc"));

// ------------------------------------------------------------ simple fields
const TEXT = ["plasma_width", "mode", "bitdepth", "main_monitor", "main_x", "main_y", "main_w", "main_h", "mfd_layout", "mfd_tint",
  "wall_cols", "radar_rotate", "pad_index", "pad_deadzone", "ramp_ms", "capture_mouse", "vsync", "max_fps",
  "print_to_file", "print_folder", "console_addr", "joy_deadzone", "radar_monitor"];
const CHECK = ["keep_aspect", "show_surface", "plasma", "plasma_ranks", "pad_enabled", "pad_invert_y", "gpu_present", "frame_stats",
  "start_printer", "joy_enabled", "mfd_windows", "plasma_window"];

function fillFields() {
  const opts = (sel, items) => { $(sel).innerHTML = items.map(([v, t]) => `<option value="${v}">${t}</option>`).join(""); };
  opts("#mode", S.modes);
  const mons = S.monitors.map(m => [m.n, `Monitor ${m.n}${m.primary ? " (main)" : ""} - ${m.w} x ${m.h}`]);
  opts("#main_monitor", mons);
  opts("#arrange_mon", mons);
  if (S.monitors.length > 1) $("#arrange_mon").value = S.monitors[1].n;
  opts("#radar_monitor", [["", "In its window (Cockpit windows, below)"],
    ...S.monitors.map(m => [String(m.n), `All of monitor ${m.n}${m.primary ? " (main)" : ""} - ${m.w} x ${m.h}`]),
    ["off", "Off: no radar window"]]);

  for (const id of TEXT) { const el = $("#" + id); if (el) el.value = V[id] ?? ""; }
  for (const id of CHECK) $("#" + id).checked = !!V[id];
  $("#main_fullscreen").value = V.main_fullscreen ? "1" : "0";
  $$("input[name=role]").forEach(r => r.checked = r.value === V.role);
  labels();
}
function labels() {
  $("#pad_deadzone_v").textContent = Math.round(V.pad_deadzone / 327.67) + "%";
  $("#joy_deadzone_v").textContent = V.joy_deadzone + "%";
  $("#ramp_ms_v").textContent = V.ramp_ms == 0 ? "off: full at once" : (V.ramp_ms / 1000).toFixed(2) + " s";
  const full = V.main_fullscreen;
  $$(".only-full").forEach(e => e.style.display = full ? "" : "none");
  $$(".only-win").forEach(e => e.style.display = full ? "none" : "");
  $$(".only-wall").forEach(e => e.style.display = V.mfd_layout === "wall" ? "" : "none");
  $$(".only-plasma").forEach(e => e.style.display = V.plasma ? "" : "none");
  // The view's shape against the window's: a wide view in a 4:3 window is a thin band.
  const [gw, gh] = V.mode.split("x").map(Number), r = mainRect();
  const off = Math.abs((r.w / r.h) / (gw / gh) - 1) > 0.05;
  $("#shape_hint").textContent = !off ? "" : V.main_fullscreen
    ? `The monitor is ${r.w} x ${r.h}, a different shape from the view: ${V.keep_aspect ? "black bars at the sides or top and bottom" : "the view is stretched"}.`
    : `The window (${r.w} x ${r.h}) is a different shape from the view (${gw} x ${gh}): ${V.keep_aspect ? "black bars fill the rest" : "the view is stretched"}. ` +
      `A window of ${r.w} x ${Math.round(r.w * gh / gw)} fits it, or show it full screen.`;
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
  for (const id of CHECK) $("#" + id).addEventListener("change", e => { V[id] = e.target.checked; labels(); renderWindows(); touch(); });
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
    .filter(w => !/^mfd/.test(w.sec) || V.mfd_windows !== false)
    .filter(w => w.sec !== "radar" || V.radar_monitor !== "off")
    .filter(w => w.sec !== "plasma" || (V.plasma && V.plasma_window !== false));
}
// The monitor a window fills ([section] monitor), or null.
function wholeMonitor(sec) {
  if (sec !== "radar" || !/^\d+$/.test(V.radar_monitor || "")) return null;
  return S.monitors.find(m => m.n == V.radar_monitor) || null;
}
function renderWindows() {
  const tb = $("#wintable tbody");
  tb.innerHTML = visibleWindows().map(w => {
    const v = V.windows[w.sec] || {}, mon = wholeMonitor(w.sec);
    if (mon) return `<tr><td>${w.title}</td><td colspan="5" class="hint">All of monitor ${mon.n}: ${mon.w} x ${mon.h} at ${mon.x}, ${mon.y}, no border</td></tr>`;
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
    const v = V.windows[w.sec] || {}, mon = wholeMonitor(w.sec);
    const d = DEF[w.sec] || [0, 0], sz = SIZE[w.sec] || [w.w, w.h];
    const placed = !!mon || (set(v.x) && set(v.y));
    const x = mon ? mon.x : placed ? +v.x : area[0] + d[0], y = mon ? mon.y : placed ? +v.y : area[1] + d[1];
    const ww = mon ? mon.w : set(v.w) ? +v.w : sz[0], hh = mon ? mon.h : set(v.h) ? +v.h : sz[1];
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
  const pw = Math.min(m.w - rw, 1344), ph = Math.floor(pw * 32 / (+V.plasma_width || 128));
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
    const joy = b.joy ? `<span class="key" data-jclr="${i}" title="Remove">${joyLabel(b.joy)}</span>`
      : `<span class="key add${jlearn && jlearn.i === i ? " listening" : ""}" data-jlearn="${i}">${jlearn && jlearn.i === i ? "press it" : "+"}</span>`;
    return `<tr><td>${b.label}</td><td><div class="keys">${chips}${add}</div></td><td><select data-pad="${i}">${opts}</select></td><td><div class="keys">${joy}</div></td></tr>`;
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
  $$(".key[data-jclr]", tb).forEach(el => el.addEventListener("click", () => { V.binds[el.dataset.jclr].joy = ""; renderBinds(); touch(); }));
  $$(".key[data-jlearn]", tb).forEach(el => el.addEventListener("click", () => {
    jlearn = { kind: "bind", i: +el.dataset.jlearn, base: snapshot() }; renderBinds();
    status("Press the controller's button, or push its hat (Esc cancels)", true);
  }));
}

// ------------------------------------------------------------ USB controllers
// Read live from /api/joy while the Controls page is open; an input is named
// as the shim's [joy] takes it: CONTROLLER.AXIS, CONTROLLER.Bn, CONTROLLER.HatUp.
const JOY_AXES = [
  ["throttle", "Throttle lever", "push the throttle from closed to fully open"],
  ["pedal_left", "Left pedal", "press the left pedal all the way down"],
  ["pedal_right", "Right pedal", "press the right pedal all the way down"],
  ["stick_x", "Stick left / right (torso twist)", "push the stick to the right"],
  ["stick_y", "Stick forward / back (aim)", "pull the stick back towards you"],
];
let J = [];            // the controllers, as last read
let jlearn = null;     // being learned: { kind: "axis", key } or { kind: "bind", i }, with the starting state
const devSpec = d => J.filter(x => x.ids === d.ids).length > 1 ? "#" + d.n : d.ids;
function findDev(spec) {
  if (!spec) return null;
  if (spec[0] === "#") return J.find(d => d.n === +spec.slice(1));
  if (/^[0-9a-f]{4}:[0-9a-f]{4}$/i.test(spec)) return J.find(d => d.ids.toUpperCase() === spec.toUpperCase());
  return J.find(d => d.name.toLowerCase().includes(spec.toLowerCase()));
}
function parseRef(s) {
  s = (s || "").trim();
  const neg = s.startsWith("-"); if (neg) s = s.slice(1);
  const i = s.lastIndexOf(".");
  return i < 0 ? null : { neg, dev: s.slice(0, i), input: s.slice(i + 1) };
}
function joyLabel(ref) {
  const r = parseRef(ref); if (!r) return ref;
  const d = findDev(r.dev);
  const input = /^B\d+$/i.test(r.input) ? "button " + r.input.slice(1) : /^hat/i.test(r.input) ? "hat " + r.input.slice(3).toLowerCase() : r.input;
  return `${d ? d.name : r.dev} ${input}${r.neg ? " (reversed)" : ""}`;
}
function axisNow(ref) {
  const r = parseRef(ref); if (!r) return null;
  const d = findDev(r.dev), a = r.input.toUpperCase();
  if (!d || !(a in d.axes)) return null;
  return r.neg ? 1000 - d.axes[a] : d.axes[a];
}
const snapshot = () => J.map(d => ({ n: d.n, axes: { ...d.axes }, buttons: [...d.buttons], hat: d.hat }));
function hatDir(pov) {
  if (pov === null || pov === undefined) return null;
  return pov >= 31500 || pov <= 4500 ? "HatUp" : pov < 13500 ? "HatRight" : pov <= 22500 ? "HatDown" : "HatLeft";
}
function renderJoyRows() {
  $("#joytable tbody").innerHTML = JOY_AXES.map(([k, label]) => `<tr>
    <td>${label}</td>
    <td><input data-jaxis="${k}" value="${V.joy_axes[k] || ""}" placeholder="not used"></td>
    <td><span class="meter"><i id="jm-${k}" style="width:0"></i></span> <span class="hint" id="jn-${k}"></span></td>
    <td><button class="btn ghost" data-jaxlearn="${k}">${jlearn && jlearn.key === k ? "Move it..." : "Learn"}</button></td></tr>`).join("");
  $$("input[data-jaxis]").forEach(el => el.addEventListener("input", () => { V.joy_axes[el.dataset.jaxis] = el.value.trim(); touch(); }));
  $$("button[data-jaxlearn]").forEach(el => el.addEventListener("click", () => {
    const k = el.dataset.jaxlearn;
    jlearn = { kind: "axis", key: k, base: snapshot() }; renderJoyRows();
    status(`Now ${JOY_AXES.find(a => a[0] === k)[2]} (Esc cancels)`, true);
  }));
}
function renderJoyDevs() {
  $("#joy_devs").innerHTML = J.length ? J.map(d => `<div class="joydev"><b>${d.name}</b><span class="pill">${d.ids}</span><span class="hint">#${d.n}</span>
    ${Object.entries(d.axes).map(([a, v]) => `<span class="joyaxis">${a} <span class="meter"><i style="width:${v / 10}%"></i></span></span>`).join("")}
    ${Array.from({ length: Math.min(d.nbuttons, 32) }, (_, b) => `<span class="joybtn${d.buttons.includes(b + 1) ? " on" : ""}">${b + 1}</span>`).join("")}
    ${d.has_hat ? `<span class="joybtn${d.hat !== null ? " on" : ""}">hat ${d.hat !== null ? hatDir(d.hat).slice(3).toLowerCase() : "-"}</span>` : ""}</div>`).join("")
    : '<p class="hint">No USB controller connected.</p>';
  for (const [k] of JOY_AXES) {
    const v = axisNow(V.joy_axes[k]), m = $("#jm-" + k);
    if (!m) continue;
    m.style.width = v === null ? "0" : v / 10 + "%";
    $("#jn-" + k).textContent = !V.joy_axes[k] ? "" : v === null ? "not connected" : Math.round(v / 10) + "%";
  }
}
function learnStep() {
  if (!jlearn) return;
  if (jlearn.kind === "axis") {
    let best = null;
    for (const d of J) {
      const b = jlearn.base.find(x => x.n === d.n); if (!b) continue;
      for (const [a, v] of Object.entries(d.axes)) {
        const delta = v - (b.axes[a] ?? v);
        if (Math.abs(delta) > 350 && (!best || Math.abs(delta) > Math.abs(best.delta))) best = { d, a, delta };
      }
    }
    if (!best) return;
    const k = jlearn.key;
    V.joy_axes[k] = (best.delta < 0 ? "-" : "") + devSpec(best.d) + "." + best.a;
    jlearn = null; renderJoyRows(); touch();
    status(`${JOY_AXES.find(a => a[0] === k)[1]}: ${joyLabel(V.joy_axes[k])} - unsaved`, true);
    return;
  }
  for (const d of J) {
    const b = jlearn.base.find(x => x.n === d.n); if (!b) continue;
    const btn = d.buttons.find(x => !b.buttons.includes(x));
    const hat = b.hat === null ? hatDir(d.hat) : null;
    if (btn || hat) {
      V.binds[jlearn.i].joy = devSpec(d) + "." + (btn ? "B" + btn : hat);
      jlearn = null; renderBinds(); touch();
      return;
    }
  }
}
async function pollJoy() {
  if (!S || !$("#page-controls").classList.contains("on")) return;
  try { J = (await api("/api/joy")).joysticks; } catch (e) { return; }
  learnStep();
  renderJoyDevs();
}
setInterval(pollJoy, 150);
document.addEventListener("keydown", e => {
  if (jlearn && e.code === "Escape") { jlearn = null; renderJoyRows(); renderBinds(); status("Cancelled", dirty); }
});
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
  J = S.joysticks || [];
  fillFields(); renderPC(); renderWindows(); renderBinds(); renderJoyRows(); renderJoyDevs(); renderTeslas(); drawMap();
  if (S.mechs && !$("#ed-name").value) edit(null);
  dirty = false;
  status(S.found ? "Ready." : "No MW4.exe in the game folder - start the launcher with --game.", !S.found);
}
async function save() {
  const r = await api("/api/save", V);
  dirty = !r.ok;
  const more = r.ok && r.message && r.message !== "Saved." ? " " + r.message.replace(/^Saved\. */, "") : "";
  status(r.ok ? "Saved to shim.ini, options.ini and ctcl.ini (the old ones are kept as .bak)." + more : "Error: " + r.error, !r.ok);
}

$("#save").addEventListener("click", save);
$("#revert").addEventListener("click", load);
$("#arrange").addEventListener("click", arrange);
$("#binds_reset").addEventListener("click", () => {
  V.binds.forEach(b => { b.keys = b.default_keys; b.pad = b.default_pad; b.joy = ""; }); renderBinds(); touch();
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

// ------------------------------------------------------------ MFD boards
// Through /api/mfd (launcher/mfdtest.py): find the boards on the serial
// ports, name each by its MFD, loop a test picture on them.
let M = null;          // the MFD boards' state, as last read
function renderMfd() {
  if (!M) return;
  const busy = $("#page-mfds").classList.contains("busy");
  const counts = {};
  M.boards.forEach(b => { if (b.mfd) counts[b.mfd] = (counts[b.mfd] || 0) + 1; });
  const off = busy || M.flashing ? "disabled" : "";
  const fw = (port, text) => {            // the firmware cell: what it runs, or how its flashing goes
    const j = M.flash[port];
    if (!j) return text;
    if (j.error) return `<span class="bad">Flashing failed: ${j.error}</span>`;
    return j.done ? "Flashed: " + text : `${j.text}... ${Math.round(j.fraction * 100)}%`;
  };
  const outdated = b => M.firmware && b.firmware !== M.firmware;
  const flashBtn = (port, label) => M.firmware_found
    ? `<button class="btn ghost" data-mfdflash="${port}" ${off}>${label}</button>` : "";
  const rows = M.boards.map(b => `<tr>
      <td>${b.port}</td><td class="mono">${b.tint ? `<span class="swatch" style="background:#${b.tint}" title="Its colour: #${b.tint}"></span>` : ""}${b.uid}</td>
      <td class="hint">${fw(b.port, b.firmware + (outdated(b) ? ` (this launcher has ${M.firmware})` : ""))}</td>
      <td><select data-mfdport="${b.port}" ${off}>
        <option value="0">-</option>${[1, 2, 3, 4, 5].map(n => `<option value="${n}" ${b.mfd === n ? "selected" : ""}>MFD ${n}</option>`).join("")}
      </select></td>
      <td>${flashBtn(b.port, outdated(b) ? "Update" : "Flash again")}</td></tr>`)
    .concat(M.new.map(d => `<tr>
      <td>${d.port}</td><td>New board <span class="hint">${d.desc.replace(/ \(COM\d+\)$/, "")}</span></td>
      <td class="hint">${fw(d.port, "no MFD firmware")}</td><td></td>
      <td>${flashBtn(d.port, "Flash")}</td></tr>`));
  $("#mfdtable tbody").innerHTML = rows.length ? rows.join("")
    : `<tr><td colspan="5" class="hint">${M.available ? "No boards found yet: Find boards." : "This launcher has no pyserial: MFD boards cannot be reached from it."}</td></tr>`;
  $$("select[data-mfdport]").forEach(el => el.addEventListener("change", () =>
    mfdCall("/api/mfd/assign", { port: el.dataset.mfdport, mfd: +el.value }, `Telling ${el.dataset.mfdport} it is ${+el.value ? "MFD " + el.value : "no MFD"}...`)));
  $$("button[data-mfdflash]").forEach(el => el.addEventListener("click", () => mfdFlash([el.dataset.mfdflash])));
  const fresh = M.new.map(d => d.port).filter(p => !M.flash[p]);
  $("#mfd_flash_new").style.display = fresh.length && M.firmware_found ? "" : "none";
  $("#mfd_flash_new").textContent = fresh.length > 1 ? `Flash all ${fresh.length} new boards` : "Flash the new board";
  $("#mfd_flash_new").dataset.ports = fresh.join(",");
  $("#mfd_test").textContent = M.testing ? "Stop test pictures" : "Show test pictures";
  $("#mfd_stream").checked = M.stream;
  $("#mfd_bridge").textContent = M.bridge ? "Stop the bridge" : "Start the bridge";
  $("#mfd_bridge").disabled = busy || (!M.bridge && (!M.bridge_found || !M.bridge_ports.length));
  $("#mfd_bridge_auto").checked = M.bridge_auto;
  $("#mfd_bridge_state").textContent = !M.bridge_found ? "The bridge (bridge.py) is not with this launcher."
    : !M.bridge_ports.length ? "No board is an MFD yet: find the boards and give each its MFD."
    : (M.bridge ? "Running, on " : "Stopped. It opens ") + M.bridge_ports.join(", ") + "."
      + (M.stream ? "" : " The game does not stream its MFDs yet: tick the box under Test pictures.");
  $("#mfd_bridge_log").textContent = (M.bridge_log || []).join("\n");
  $("#mfd_rec_state").textContent = M.recorded.length
    ? `Recorded from the game: MFD ${M.recorded.join(", ")}. The others are drawn.`
    : "No recordings: every MFD's test picture is drawn.";
  const missing = [1, 2, 3, 4, 5].filter(n => !counts[n]);
  if (M.flashing) $$("#page-mfds button").forEach(b => b.disabled = true);
  $("#mfd_hint").textContent = M.errors.length ? "Trouble: " + M.errors.join("; ")
    : M.boards.length ? (missing.length ? `Not on a board yet: MFD ${missing.join(", ")}.` : "All five MFDs have a board.")
    : "Finding boards briefly opens every serial port.";
}
async function mfdCall(path, body, doing) {
  const page = $("#page-mfds");
  page.classList.add("busy");
  status(doing, true);
  $$("#page-mfds button").forEach(b => b.disabled = true);
  try {
    const r = await api(path, body || {});
    if (r.error) status("Error: " + r.error, true, true);
    else { M = r; status(r.message || "Done.", dirty); }
  } finally {
    page.classList.remove("busy");
    $$("#page-mfds button").forEach(b => b.disabled = false);
    renderMfd();
  }
}
async function loadMfd() {
  M = await api("/api/mfd");
  renderMfd();
  if (M.flashing) followFlash();
}
// Flashing runs in the launcher; the page follows it, then finds the boards
// again - each one flashed answers as an MFD board from then on.
async function mfdFlash(ports) {
  if (!ports.length) return;
  if (!confirm(`Put the MFD firmware (${M.firmware}) on ${ports.join(", ")}?\n\nWhatever the board ran before is replaced. Keep it plugged in until it is done.`)) return;
  await mfdCall("/api/mfd/flash", { ports }, `Flashing ${ports.join(", ")}...`);
  if (M.flashing) followFlash();
}
let flashTimer = null;
function followFlash() {
  if (flashTimer) return;
  status("Flashing the boards: keep them plugged in...", true);
  flashTimer = setInterval(async () => {
    M = await api("/api/mfd");
    renderMfd();
    if (M.flashing) return;
    clearInterval(flashTimer);
    flashTimer = null;
    const flash = M.flash, ports = Object.keys(flash), bad = ports.filter(p => flash[p].error);
    await new Promise(r => setTimeout(r, 2500));           // the boards starting their firmware
    await mfdCall("/api/mfd/scan", {}, "Finding the boards again...");
    M.flash = flash;
    renderMfd();
    status(bad.length ? `Flashing failed on ${bad.join(", ")}: see the table.`
      : `Flashed ${ports.join(", ")}: give each board its MFD.`, dirty, !!bad.length);
  }, 1000);
}
$("#mfd_flash_new").addEventListener("click", e => mfdFlash(e.target.dataset.ports.split(",").filter(Boolean)));
$("#mfd_scan").addEventListener("click", () => mfdCall("/api/mfd/scan", {}, "Looking for MFD boards..."));
$("#mfd_test").addEventListener("click", () => mfdCall("/api/mfd/test", { on: !(M && M.testing) },
  M && M.testing ? "Stopping the test pictures..." : "Starting the test pictures..."));
$("#mfd_record").addEventListener("click", () => mfdCall("/api/mfd/record", { seconds: 4 }, "Recording the MFDs from the game..."));
$("#mfd_forget").addEventListener("click", () => mfdCall("/api/mfd/forget", {}, "Deleting the recordings..."));
$("#mfd_stream").addEventListener("change", e => mfdCall("/api/mfd/stream", { on: e.target.checked }, "Saving..."));
$("#mfd_bridge").addEventListener("click", () => mfdCall("/api/mfd/bridge", { on: !(M && M.bridge) },
  M && M.bridge ? "Stopping the bridge..." : "Starting the bridge..."));
$("#mfd_bridge_auto").addEventListener("change", e => mfdCall("/api/mfd/bridge", { auto: e.target.checked }, "Saving..."));

// ------------------------------------------------------------ careers and leaderboards
// Through /api/career (launcher/career.py): every pilot's matches, as the
// pods report them to the console, summed into ranks and standings.
const RANK_CLASS = r => r >= 9 ? "gen" : r >= 5 ? "off" : r >= 4 ? "vet" : r >= 1 ? "enl" : "";
function rankBadge(c, always) {
  if (!c || (!c.career_matches && !always)) return "";
  const pips = c.rank >= 5 ? Math.min(c.rank - 4, 5) : 0;
  return ` <span class="rk ${RANK_CLASS(c.rank)}" title="${esc(c.title)}">${esc(c.short)}${"<i class=\"pip\"></i>".repeat(pips)}</span>`;
}
function careerOf(id) { return P && P.careers ? P.careers[id] : null; }
const num = v => (v ?? 0).toLocaleString();
function rankBar(c) {
  if (!c.next) return `<div class="bar"><i style="width:100%"></i></div><div class="bar-note">The highest rank</div>`;
  return `<div class="bar"><i style="width:${Math.round(c.progress * 100)}%"></i></div>
    <div class="bar-note">${num(c.xp)} / ${num(c.next_xp)} experience to ${esc(c.next)}</div>`;
}
function renderCareer(p) {
  const el = $("#ed-career"), c = p && careerOf(p.id);
  el.hidden = !c;
  if (!c) return;
  el.innerHTML = `<div class="title">${rankBadge(c, true)} ${esc(c.title)}${c.veteran ? ` <span class="hint">veteran</span>` : ""}</div>
    ${rankBar(c)}
    <div class="stats">
      <div><b>${num(c.matches)}</b><span>matches</span></div><div><b>${num(c.wins)}</b><span>wins</span></div>
      <div><b>${num(c.kills)}</b><span>kills</span></div><div><b>${num(c.deaths)}</b><span>deaths</span></div>
      <div><b>${num(c.score)}</b><span>total score</span></div><div><b>${c.best == null ? "-" : num(c.best)}</b><span>best match</span></div>
      <div><b>${num(c.avg)}</b><span>average</span></div><div><b>${c.matches ? c.kd.toFixed(2) : "-"}</b><span>kills / death</span></div>
    </div>`;
}

let LB = null;
function sinceDate(v) {                // this PC's own calendar, as the records' times are
  const d = new Date(), ymd = x => `${x.getFullYear()}-${String(x.getMonth() + 1).padStart(2, "0")}-${String(x.getDate()).padStart(2, "0")}`;
  if (v === "today") return ymd(d);
  if (v === "week") { d.setDate(d.getDate() - 6); return ymd(d); }
  if (v === "month") { d.setDate(1); return ymd(d); }
  return "";
}
async function loadBoards() {
  const since = sinceDate($("#lb-since").value);
  const game = $("#lb-game").value, q = [since && "since=" + since, game && "game=" + game].filter(Boolean).join("&");
  LB = await api("/api/career" + (q ? "?" + q : ""));
  const err = $("#lb-error");
  err.hidden = !LB.error;
  if (LB.error) {
    err.textContent = LB.error + " - careers live on the console; this pod reaches them through the console's launcher.";
    return;
  }
  renderBoards();
}
function renderBoards() {
  if (!LB || LB.error) return;
  const key = $("#lb-sort").value;
  const flown = LB.pilots.filter(c => c.matches);
  const by = {
    xp: c => [c.xp, c.score], score: c => [c.score, c.xp], kills: c => [c.kills, c.kd], kd: c => [c.kd, c.kills],
    wins: c => [c.wins, c.score], best: c => [c.best ?? -1e9, c.score], avg: c => [c.avg, c.matches],
    matches: c => [c.matches, c.score],
  }[key];
  const cmp = (a, b) => { const x = by(a), y = by(b); return y[0] - x[0] || y[1] - x[1] || a.name.localeCompare(b.name); };
  const rows = flown.slice().sort(cmp);
  // The best of the period, at the top.
  const top = (f, min) => flown.filter(c => c.matches >= (min || 1)).slice().sort((a, b) => f(b) - f(a))[0];
  const card = (k, c, v) => `<div><div class="k">${k}</div><div class="v">${c ? v(c) : "-"}</div><div class="w">${c ? esc(c.name) + rankBadge(c) : "No matches yet"}</div></div>`;
  $("#lb-best").innerHTML =
    card("Highest rank", top(c => c.xp), c => esc(c.title)) +
    card("Most kills", top(c => c.kills), c => num(c.kills)) +
    card("Best match", top(c => c.best ?? -1e9), c => num(c.best)) +
    card("Kills per death (3+ matches)", top(c => c.kd, 3), c => c.kd.toFixed(2));
  $("#lb-table tbody").innerHTML = rows.map((c, i) => `<tr>
      <td class="pos">${i + 1}</td>
      <td class="who">${esc(c.name)}${c.profile ? "" : ` <span class="hint" title="Not on the roster">guest</span>`}</td>
      <td class="rank">${rankBadge(c, true)} ${esc(c.title)}${c.next ? `<div class="bar"><i style="width:${Math.round(c.progress * 100)}%"></i></div>` : ""}</td>
      <td class="n">${num(c.matches)}</td><td class="n">${num(c.wins)}</td><td class="n">${num(c.kills)}</td><td class="n">${num(c.deaths)}</td>
      <td class="n">${c.kd.toFixed(2)}</td><td class="n">${num(c.score)}</td><td class="n">${c.best == null ? "-" : num(c.best)}</td>
      <td class="n">${num(c.avg)}</td><td>${esc(c.mech)}</td><td class="hint">${esc(c.last)}</td></tr>`).join("")
    || `<tr><td colspan="13" class="hint">No matches ${LB.since ? "in this time" : "yet"}: they appear here once a pod has flown one and its launcher has passed it on.</td></tr>`;
  const place = m => m.of > 1 ? `${m.place} of ${m.of}` : "-";
  $("#lb-recent tbody").innerHTML = (LB.recent || []).map(m => `<tr>
      <td class="hint">${esc(m.time)}</td><td>${esc(m.name)}</td><td>${esc(m.mission || m.shown)}${m.game ? ` <span class="hint">${esc(m.game)}</span>` : ""}</td>
      <td>${esc(m.mech)}</td><td class="n">${num(m.score)}</td><td class="n">${m.kills} / ${m.deaths}</td><td class="n">${place(m)}</td></tr>`).join("")
    || `<tr><td colspan="7" class="hint">None yet.</td></tr>`;
  $("#lb-ranks").innerHTML = `<div class="ladder">` + LB.ranks.map(r =>
    `<span>${rankBadge({ rank: r.rank, short: r.short, title: r.title }, true)}</span><span>${esc(r.title)}</span><span class="x">${r.rank ? num(r.xp) : "no matches"}</span>`).join("") + `</div>`;
}
$("#lb-since").addEventListener("change", loadBoards);
$("#lb-game").addEventListener("change", loadBoards);
$("#lb-sort").addEventListener("change", renderBoards);

// Keep the server alive while the window is open; refresh the launch page.
// If it cannot be reached the page says so: everything on it needs it.
let lost = 0;
function ping() {
  fetch("/api/ping").then(r => {
    if (!r.ok) throw new Error(r.status);
    if (lost) { lost = 0; status("Ready.", dirty); }
  }).catch(() => {
    if (++lost >= 2) status("The launcher is not running - close this window and start it again.", true, true);
  });
}
setInterval(ping, 2000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) ping(); });
window.addEventListener("pagehide", () => navigator.sendBeacon("/api/bye", ""));
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
// The radar's damage diagram, for a 'Mech the game has no portrait of.
function mechSchematic(chassis) {
  const m = (P && P.mechs) || (S && S.mechs) || {};
  const f = (m.schematics || {})[chassis];
  return f ? "/game/" + f.split("/").map(encodeURIComponent).join("/") : "";
}
function portrait() {
  const c = $("#ed-chassis").value, v = $("#ed-variant");
  const img = mechImage(c), sch = img ? "" : mechSchematic(c), el = $("#ed-portrait");
  el.classList.toggle("none", !img && !sch);
  el.classList.toggle("schematic", !!sch);
  el.dataset.msg = img || sch ? "" : "No picture of the " + c + " in the game's files";
  el.style.backgroundImage = img || sch ? `url("${img || sch}")` : "";
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
    ? "Everyone who flies here. Seat a pilot in a pod: when the console starts it puts them in a slot, and at LOAD the pod gets their name and 'Mech."
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
    let action;
    if (!P.local)
      action = `<button class="btn ${here ? "ghost" : "primary"}" data-sit="${p.id}">${here ? "Leave" : "Sit here"}</button>`;
    else if (seat)                                      // the console: seat or unseat them right here
      action = `<button class="btn ghost" data-seat-pod="${esc(seat[0])}" data-seat-id="">Unseat</button>`;
    else if ((P.pods || []).length === 1)
      action = `<button class="btn primary" data-seat-pod="${esc(P.pods[0].ip)}" data-seat-id="${p.id}">Seat in ${esc(P.pods[0].slot)}</button>`;
    else if ((P.pods || []).length > 1)
      action = `<select class="seat-in" data-seat-for="${p.id}"><option value="">Seat in...</option>` +
        P.pods.map(t => `<option value="${esc(t.ip)}">${esc(t.slot)}</option>`).join("") + `</select>`;
    else action = "";
    return `<div class="pilot${p.id === selected ? " sel" : ""}" data-id="${p.id}">
      <div class="avatar${mechImage(p.chassis) ? "" : mechSchematic(p.chassis) ? " schematic" : " none"}" style="${(mechImage(p.chassis) || mechSchematic(p.chassis)) ? `background-image:url('${mechImage(p.chassis) || mechSchematic(p.chassis)}')` : ""}"></div>
      <div class="who"><div class="nm">${esc(p.name)}${rankBadge(careerOf(p.id))}</div><div class="mc">${esc(mechLabel(p))}</div></div>
      ${seat ? `<div class="seat-tag">${here ? "in this pod" : "in " + esc(podName(seat[0]))}</div>` : ""}
      ${action}</div>`;
  }).join("") || `<div class="empty">No pilots yet - make the first on the right.</div>`;
  $$(".pilot", $("#roster")).forEach(el => el.addEventListener("click", e => {
    if (e.target.dataset.sit || e.target.dataset.seatPod || e.target.dataset.seatFor) return;
    selected = el.dataset.id; edit(P.roster.find(p => p.id === selected)); renderRoster();
  }));
  const seatIn = async (pod, id) => {
    const r = await api("/api/seat", { pod, id: id || null });
    status(r.ok ? (r.profile ? `${r.profile.name} seated in ${podName(pod)}: the console fills in their slot when it starts.`
                             : `${podName(pod)} emptied.`) : "Error: " + r.error, !r.ok);
    loadPilots();
  };
  $$("[data-seat-pod]", $("#roster")).forEach(el => el.addEventListener("click", e => {
    e.stopPropagation(); seatIn(el.dataset.seatPod, el.dataset.seatId);
  }));
  $$("select[data-seat-for]", $("#roster")).forEach(el => {
    el.addEventListener("click", e => e.stopPropagation());
    el.addEventListener("change", () => el.value && seatIn(el.value, el.dataset.seatFor));
  });
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
  // A seat written by something else (a tool, a hand edit) names a pilot who
  // is not on the roster: show it as it is, so the pod is not taken for empty.
  const opts = s => {
    const id = s && s.profile, known = id && P.roster.some(p => p.id === id);
    return `<option value="">- empty -</option>` +
      (s && (s.name || s.mech) && !known ? `<option value="" selected>${esc(s.name || "?")} - ${esc(s.mech || "")} (not on the roster)</option>` : "") +
      P.roster.slice().sort((a, b) => a.name.localeCompare(b.name)).map(p => `<option value="${p.id}" ${p.id === id ? "selected" : ""}>${esc(p.name)} - ${esc(mechLabel(p))}</option>`).join("");
  };
  $("#seats").innerHTML = (P.pods || []).map(t => {
    const s = (P.seats || {})[t.ip];
    return `<div class="seat"><div class="dot${s ? "" : " off"}"></div><div class="pod"><b>${esc(t.slot)}</b><div class="hint">${esc(t.ip)}</div></div>
      <select data-pod="${esc(t.ip)}">${opts(s)}</select></div>`;
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
// Camo and unit: the console's numbered lists, as pictures where
// tools/livery.py has read them off the console, else as numbers.
let edCamo = 1, edUnit = 1;
function mechs() { return (P && P.mechs) || (S && S.mechs) || {}; }
function swatches(id, kind, count, cur, set) {
  const pics = (mechs().livery || {})[kind] || {};
  $(id).innerHTML = Array.from({ length: count }, (_, i) => i + 1).map(n => pics[n]
    ? `<button type="button" class="sw${n === cur ? " on" : ""}" data-n="${n}" title="${kind} ${n}" style="background-image:url('/game/${pics[n]}')"><span>${n}</span></button>`
    : `<button type="button" class="sw num${n === cur ? " on" : ""}" data-n="${n}" title="${kind} ${n}"><span>${n}</span></button>`).join("");
  $$(".sw", $(id)).forEach(b => b.addEventListener("click", () => { set(+b.dataset.n); }));
}
function renderLivery() {
  const L = mechs().livery || { camos: 16, units: 17 };
  swatches("#ed-camo", "camo", L.camos, edCamo, n => { edCamo = n; renderLivery(); });
  swatches("#ed-unit", "unit", L.units, edUnit, n => { edUnit = n; renderLivery(); });
}
function edit(p) {
  selected = p ? p.id : null;
  edCamo = p && p.camo || 1;
  edUnit = p && p.unit || 1;
  renderLivery();
  $("#ed-title").textContent = p ? "Edit " + p.name : "New pilot";
  $("#ed-name").value = p ? p.name : "";
  fillChassis(p ? p.chassis : "Loki");
  fillVariants(p ? p.variant : "");
  $("#ed-notes").value = p ? p.notes || "" : "";
  $("#ed-delete").disabled = !p;
  renderCareer(p);
}
function wirePilots() {
  $("#pilot_filter").addEventListener("input", () => P && renderRoster());
  $("#ed-chassis").addEventListener("change", () => fillVariants(""));
  $("#ed-variant").addEventListener("change", portrait);
  $("#ed-new").addEventListener("click", () => { edit(null); if (P) renderRoster(); $("#ed-name").focus(); });
  $("#ed-save").addEventListener("click", async () => {
    const body = { id: selected, name: $("#ed-name").value, chassis: $("#ed-chassis").value, variant: $("#ed-variant").value,
                   notes: $("#ed-notes").value, camo: edCamo, unit: edUnit };
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
