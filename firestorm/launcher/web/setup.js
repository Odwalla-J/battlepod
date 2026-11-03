// The first-time setup: what this PC is, and for a pod its number, screens
// and controls. Reads /api/setup, writes it with POST /api/setup; the
// launcher then turns a pod into the unattended pod (podagent.py).
"use strict";
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const esc = t => String(t ?? "").replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const api = (path, body) => fetch(path, body === undefined ? {} : {
  method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
}).then(r => r.json());

let S = null;                 // what the launcher said
const A = {};                 // the answers
let at = 0;
const TITLES = { role: "This PC", name: "Pod", screens: "Screens", controls: "Controls", network: "Network", finish: "Finish" };
const steps = () => A.role === "console" ? ["role", "network", "finish"] : ["role", "name", "screens", "controls", "network", "finish"];

function msg(t, err) { const m = $("#msg"); m.textContent = t || ""; m.classList.toggle("err", !!err); }

function go(i) {
  const list = steps();
  at = Math.max(0, Math.min(list.length - 1, i));
  const cur = list[at];
  $$(".step").forEach(s => s.classList.toggle("on", s.dataset.step === cur));
  $("#steps").innerHTML = list.map((k, n) => `<span class="${n === at ? "on" : n < at ? "done" : ""}">${n + 1}. ${TITLES[k]}</span>`).join("");
  $("#back").style.visibility = at ? "" : "hidden";
  $("#next").textContent = cur === "finish" ? "Finish" : "Next";
  $$(".only-console").forEach(e => e.style.display = A.role === "console" ? "" : "none");
  msg("");
  if (cur === "screens") drawMons();
  if (cur === "network") network();
  if (cur === "finish") finishHint();
}

// ---- 2: the pod's number and name
function fillName() {
  $("#num").innerHTML = Array.from({ length: 16 }, (_, i) => `<option value="${i + 1}">${i + 1}</option>`).join("");
  A.number = S.number || "1";
  A.name = S.name || "";
  $("#num").value = A.number;
  $("#name").value = A.name || "Pod " + A.number;
  $("#host").textContent = S.host;
  $("#num").addEventListener("change", e => {
    const was = "Pod " + A.number;
    A.number = e.target.value;
    if (!$("#name").value || $("#name").value === was) $("#name").value = "Pod " + A.number;
  });
}

// ---- 3: which monitor is which
function drawMons() {
  const box = $("#mons"), ms = S.monitors;
  const x0 = Math.min(...ms.map(m => m.x)), y0 = Math.min(...ms.map(m => m.y));
  const x1 = Math.max(...ms.map(m => m.x + m.w)), y1 = Math.max(...ms.map(m => m.y + m.h));
  const k = Math.min((box.clientWidth || 900) / (x1 - x0), 250 / (y1 - y0));
  box.innerHTML = ms.map(m => {
    const cls = m.n == A.main ? "main" : m.n == A.cockpit ? "cockpit" : "";
    const tag = cls === "main" ? "MAIN VIEW" : cls === "cockpit" ? "COCKPIT SCREENS" : "";
    return `<div class="mon ${cls}" data-n="${m.n}" style="left:${(m.x - x0) * k}px;top:${(m.y - y0) * k}px;width:${m.w * k - 6}px;height:${m.h * k - 6}px">
      <b>${m.n}</b>${m.w} x ${m.h}<span class="tag">${tag}</span></div>`;
  }).join("");
  $$(".mon", box).forEach(el => el.addEventListener("click", () => {
    const n = +el.dataset.n;
    if (n === A.main) return;
    // A click on another monitor: the main view if none yet, else the cockpit screens.
    if (n === A.cockpit) { A.cockpit = A.main; A.main = n; }
    else if (ms.length === 1) A.main = n;
    else A.cockpit = n;
    drawMons();
  }));
  $("#mon-hint").textContent = ms.length === 1
    ? "One monitor: the main view only. Firestorm's cockpit screens then open as windows over it."
    : `Main view on monitor ${A.main}${A.cockpit ? ", cockpit screens on monitor " + A.cockpit : ""}. Click the cockpit-screens monitor to swap them.`;
}

// ---- 4: the controls, live
let joyTimer = null;
async function pollJoy() {
  if (steps()[at] !== "controls") return;
  const r = await api("/api/joy").catch(() => null);
  if (!r) return;
  const js = r.joysticks || [];
  $("#devs").innerHTML = js.length ? js.map(d => {
    const live = d.buttons.length || d.hat !== null;
    const axes = Object.entries(d.axes).map(([a, v]) => `<span title="${a}" class="axis"><i style="width:${v / 10}%"></i></span>`).join("");
    return `<div class="dev"><span class="lamp ${live ? "on" : ""}"></span><div><b>${esc(d.name)}</b> <span class="hint">${d.ids}, ${d.nbuttons} buttons</span>
      <div class="axes">${axes}</div><div class="hint">${d.buttons.length ? "Button " + d.buttons.join(", ") : "&nbsp;"}</div></div></div>`;
  }).join("") : `<p class="hint">No stick, throttle or pedals found. Plug them in - they show here when Windows has them. A pod can be set up without them and checked later.</p>`;
}

// ---- 5: the network
async function network() {
  $("#net-lead").textContent = A.role === "console"
    ? "The console finds the pods by themselves: each pod tells the network it is there, and the console answers. They must be on the same network as this PC."
    : "The console finds this pod by itself once both are on the same network. Nothing to type.";
  $("#addrs").innerHTML = (S.addresses || []).map(a => `<span class="pill">${esc(a)}</span>`).join("") || '<span class="hint">no network</span>';
  if (A.role === "console") pollFound();
}
async function pollFound() {
  if (steps()[at] !== "network" || A.role !== "console") return;
  const r = await api("/api/pods").catch(() => null);
  const pods = (r && r.pods) || [];
  $("#found").innerHTML = pods.length ? pods.map(p => `<span class="pill ${p.online ? "ok" : ""}">${esc(p.name)} - ${esc(p.ip)}</span>`).join("")
    : "None yet. A pod shows here once it is set up and running (and Windows Firewall lets it through on both PCs).";
  setTimeout(pollFound, 2000);
}
$("#fw").addEventListener("click", async () => {
  $("#fw").disabled = true;
  $("#fw-msg").textContent = "Waiting for Windows...";
  const r = await api("/api/setup/firewall", {}).catch(e => ({ error: String(e) }));
  $("#fw").disabled = false;
  $("#fw-msg").textContent = r.message || r.error;
});

function finishHint() {
  $("#finish-hint").textContent = A.role === "console"
    ? "The launcher opens on its Pods page each time Windows starts. Close the launcher's window to stop it."
    : "From now on, when Windows starts this pod shows Ready on its main view and waits for the console. To change its settings later, use Settings on the console's Pods page.";
}

async function finish() {
  $("#next").disabled = true;
  msg("Saving...");
  const r = await api("/api/setup", { role: A.role, number: A.number, name: $("#name").value.trim(),
    main: A.main, cockpit: A.cockpit || 0, autostart: $("#autostart").checked }).catch(e => ({ error: String(e) }));
  $("#next").disabled = false;
  if (!r.ok) return msg(r.error || "Could not save", true);
  if (A.role === "console") { location.href = "/#pods"; return; }
  $$(".step").forEach(s => s.classList.toggle("on", s.dataset.step === "done"));
  $("#done-title").textContent = "Ready";
  $("#done-text").textContent = "This pod is set up. Its Ready screen comes up in a moment.";
  $(".nav-btns").style.display = "none";
  setTimeout(() => window.close(), 1500);
}

$("#next").addEventListener("click", () => {
  const cur = steps()[at];
  if (cur === "role" && !A.role) return msg("Pod or console?", true);
  if (cur === "name") A.name = $("#name").value.trim();
  if (cur === "finish") return finish();
  go(at + 1);
  if (steps()[at] === "controls") { clearInterval(joyTimer); joyTimer = setInterval(pollJoy, 150); pollJoy(); }
});
$("#back").addEventListener("click", () => go(at - 1));
$$("input[name=role]").forEach(r => r.addEventListener("change", () => { A.role = r.value; go(at); }));
window.addEventListener("resize", () => { if (steps()[at] === "screens") drawMons(); });

// Keep the launcher alive while this is open.
setInterval(() => fetch("/api/ping").catch(() => {}), 2000);

api("/api/setup").then(s => {
  S = s;
  A.role = s.done ? s.role : "";
  if (A.role === "both") A.role = "";
  $$("input[name=role]").forEach(r => r.checked = r.value === A.role);
  A.main = s.main;
  A.cockpit = s.cockpit || 0;
  $("#autostart").checked = s.done ? s.autostart : true;
  fillName();
  go(0);
  if (!s.found) msg("No MW4.exe in this copy's game folder - is it complete?", true);
});
