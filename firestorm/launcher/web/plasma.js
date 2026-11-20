// The Plasma sign page: the orange dot-matrix sign on top of the pod, drawn
// here as the pod's sign window draws it. The examples are the bytes the
// game sends (MW4.exe 0x569DE0-0x56A3ED: the name, then a match's place and
// score in two boxes) fed through the same interpreter, font and rank line
// as the shim's (src/ddraw.c, "the plasma sign") - so what shows here is
// what a pod shows, dot for dot.
"use strict";

const PL_GAME_W = 112, PL_H = 32;     // the game's sign; this one is [plasma] width across

// The shim's 5x7 font, ' ' to '~': five columns, bit 0 at the top.
const PL_FONT = [
  [0x00,0x00,0x00,0x00,0x00],[0x00,0x00,0x5F,0x00,0x00],[0x00,0x07,0x00,0x07,0x00],[0x14,0x7F,0x14,0x7F,0x14],[0x24,0x2A,0x7F,0x2A,0x12],[0x23,0x13,0x08,0x64,0x62],[0x36,0x49,0x55,0x22,0x50],[0x00,0x05,0x03,0x00,0x00],
  [0x00,0x1C,0x22,0x41,0x00],[0x00,0x41,0x22,0x1C,0x00],[0x08,0x2A,0x1C,0x2A,0x08],[0x08,0x08,0x3E,0x08,0x08],[0x00,0x50,0x30,0x00,0x00],[0x08,0x08,0x08,0x08,0x08],[0x00,0x60,0x60,0x00,0x00],[0x20,0x10,0x08,0x04,0x02],
  [0x3E,0x51,0x49,0x45,0x3E],[0x00,0x42,0x7F,0x40,0x00],[0x42,0x61,0x51,0x49,0x46],[0x21,0x41,0x45,0x4B,0x31],[0x18,0x14,0x12,0x7F,0x10],[0x27,0x45,0x45,0x45,0x39],[0x3C,0x4A,0x49,0x49,0x30],[0x01,0x71,0x09,0x05,0x03],
  [0x36,0x49,0x49,0x49,0x36],[0x06,0x49,0x49,0x29,0x1E],[0x00,0x36,0x36,0x00,0x00],[0x00,0x56,0x36,0x00,0x00],[0x08,0x14,0x22,0x41,0x00],[0x14,0x14,0x14,0x14,0x14],[0x00,0x41,0x22,0x14,0x08],[0x02,0x01,0x51,0x09,0x06],
  [0x32,0x49,0x79,0x41,0x3E],[0x7E,0x11,0x11,0x11,0x7E],[0x7F,0x49,0x49,0x49,0x36],[0x3E,0x41,0x41,0x41,0x22],[0x7F,0x41,0x41,0x22,0x1C],[0x7F,0x49,0x49,0x49,0x41],[0x7F,0x09,0x09,0x09,0x01],[0x3E,0x41,0x49,0x49,0x7A],
  [0x7F,0x08,0x08,0x08,0x7F],[0x00,0x41,0x7F,0x41,0x00],[0x20,0x40,0x41,0x3F,0x01],[0x7F,0x08,0x14,0x22,0x41],[0x7F,0x40,0x40,0x40,0x40],[0x7F,0x02,0x0C,0x02,0x7F],[0x7F,0x04,0x08,0x10,0x7F],[0x3E,0x41,0x41,0x41,0x3E],
  [0x7F,0x09,0x09,0x09,0x06],[0x3E,0x41,0x51,0x21,0x5E],[0x7F,0x09,0x19,0x29,0x46],[0x46,0x49,0x49,0x49,0x31],[0x01,0x01,0x7F,0x01,0x01],[0x3F,0x40,0x40,0x40,0x3F],[0x1F,0x20,0x40,0x20,0x1F],[0x3F,0x40,0x38,0x40,0x3F],
  [0x63,0x14,0x08,0x14,0x63],[0x07,0x08,0x70,0x08,0x07],[0x61,0x51,0x49,0x45,0x43],[0x00,0x7F,0x41,0x41,0x00],[0x02,0x04,0x08,0x10,0x20],[0x00,0x41,0x41,0x7F,0x00],[0x04,0x02,0x01,0x02,0x04],[0x40,0x40,0x40,0x40,0x40],
  [0x00,0x01,0x02,0x04,0x00],[0x20,0x54,0x54,0x54,0x78],[0x7F,0x48,0x44,0x44,0x38],[0x38,0x44,0x44,0x44,0x20],[0x38,0x44,0x44,0x48,0x7F],[0x38,0x54,0x54,0x54,0x18],[0x08,0x7E,0x09,0x01,0x02],[0x0C,0x52,0x52,0x52,0x3E],
  [0x7F,0x08,0x04,0x04,0x78],[0x00,0x44,0x7D,0x40,0x00],[0x20,0x40,0x44,0x3D,0x00],[0x7F,0x10,0x28,0x44,0x00],[0x00,0x41,0x7F,0x40,0x00],[0x7C,0x04,0x18,0x04,0x78],[0x7C,0x08,0x04,0x04,0x78],[0x38,0x44,0x44,0x44,0x38],
  [0x7C,0x14,0x14,0x14,0x08],[0x08,0x14,0x14,0x18,0x7C],[0x7C,0x08,0x04,0x04,0x08],[0x48,0x54,0x54,0x54,0x20],[0x04,0x3F,0x44,0x40,0x20],[0x3C,0x40,0x40,0x20,0x7C],[0x1C,0x20,0x40,0x20,0x1C],[0x3C,0x40,0x30,0x40,0x3C],
  [0x44,0x28,0x10,0x28,0x44],[0x0C,0x50,0x50,0x50,0x3C],[0x44,0x64,0x54,0x4C,0x44],[0x00,0x08,0x36,0x41,0x00],[0x00,0x00,0x7F,0x00,0x00],[0x00,0x41,0x36,0x08,0x00],[0x10,0x08,0x08,0x10,0x08],
];

// The shim's sign: the commands the game writes, and over a pilot's name
// the rank line from profiles\ranks.ini ({name: [rank, TITLE]} here).
class Sign {
  constructor(ranks, showRanks, width) {
    this.ranks = ranks || {};
    this.showRanks = showRanks !== false;
    this.w = Math.max(PL_GAME_W, Math.min(160, width || 128));
    this.dx = (this.w - PL_GAME_W) >> 1;   // the game's picture, moved to the middle
    this.dots = new Uint8Array(this.w * PL_H);
    this.cmd = []; this.need = 0; this.inText = false;
    this.cx = 0; this.cy = 0; this.font = 0; this.name = ""; this.shown = ""; this.inside = false;
    this.big = false; this.shrink = false;  // a name on the sign; the match's boxes coming under it
    this.boxes = false;                     // the match's boxes on the sign
    this.bytes = [];
  }
  dot(x, y, on) { if (x >= 0 && x < this.w && y >= 0 && y < PL_H) this.dots[y * this.w + x] = on; }
  rect(x0, y0, x1, y1, fill, on) {
    for (let y = y0; y <= y1; y++)
      for (let x = x0; x <= x1; x++)
        if (fill || y === y0 || y === y1 || x === x0 || x === x1) this.dot(x, y, on);
  }
  char(x, y, ch, big) {
    const colX = [0, 2, 4, 5, 7], colW = [2, 2, 1, 2, 2];
    this.rect(x, y, x + (big ? 9 : 5), y + (big ? 13 : 6), 1, 0);
    if (ch < 0x20 || ch > 0x7E) ch = 0x3F;
    const g = PL_FONT[ch - 0x20];
    for (let c = 0; c < 5; c++)
      for (let r = 0; r < 7; r++) {
        if (!(g[c] >> r & 1)) continue;
        if (!big) this.dot(x + c, y + r, 1);
        else for (let i = 0; i < colW[c]; i++) for (let j = 0; j < 2; j++) this.dot(x + colX[c] + i, y + 2 * r + j, 1);
      }
    return big ? 10 : 6;
  }
  // One write from the game (or the rank line's own), as plasma_feed takes
  // it: the game's x's moved across, and kept as moved (what a real sign gets).
  feed(b) {
    if (!this.inside && this.boxText(b)) return;
    for (let c of b) {
      const have = this.cmd.length, m1 = this.cmd[1];
      if (have && !this.inside && ((have === 2 && m1 === 0x52) || (m1 === 0x58 && (have === 2 || have === 4)) ||
                                   (m1 === 0x78 && (have === 3 || have === 5))))
        c = (c + this.dx) & 0xFF;
      this.bytes.push(c);
      if (this.cmd.length) {
        this.inText = false;
        this.cmd.push(c);
        if (this.cmd.length === 2)
          this.need = { 0x40: 2, 0x52: 3, 0x51: 3, 0x4B: 3, 0x48: 3, 0x47: 3, 0x58: 6, 0x78: 7 }[c] || 2;
        if (this.cmd.length < this.need) continue;
        const m = this.cmd;
        switch (m[1]) {
          case 0x40: this.dots.fill(0); this.cx = this.cy = 0; this.shown = ""; if (!this.inside) { this.name = ""; this.big = false; this.boxes = false; } break;
          case 0x52: this.cx = m[2]; break;
          case 0x51: this.cy = m[2]; break;
          case 0x4B: this.font = m[2]; break;
          case 0x58: this.rect(m[2], m[3], m[4], m[5], 0, 1); break;
          case 0x78:
            this.rect(m[3], m[4], m[5], m[6], 1, m[2] !== 0 ? 1 : 0);
            if (!this.inside && m[4] >= 16) {    // the match's boxes
              this.boxes = true;
              if (this.big) this.shrink = true;
            }
            break;
        }
        this.cmd = [];
      } else if (c === 0x1B) {
        this.cmd = [c]; this.need = 2;
      } else {
        if (!this.inside && this.cy < 16) {
          this.name = (this.inText ? this.name : "") + String.fromCharCode(c);
          this.big = true;
        }
        this.inText = true;
        this.cx = (this.cx + this.char(this.cx, this.cy, c, this.font === 4 || this.font === 5)) & 0xFF;
      }
    }
    if (!this.inside) {
      this.name = this.name.trim();
      if (this.shrink) {
        this.shrink = false;
        if (this.name) this.small();
      }
      this.rank(true);
    }
  }
  // The game's place or score in the boxes (row 20), or the spaces that rub
  // them out: in the middle of its box, or the box's inside cleared.
  boxText(b) {
    if (!this.boxes || this.cy !== 20 || !b.length || b.length > 8 || b.includes(0x1B)) return false;
    const [x0, x1] = this.cx < 42 + this.dx ? [27 + this.dx, 42 + this.dx] : [42 + this.dx, 93 + this.dx];
    let out;
    if (b.every(c => c === 0x20)) out = [0x1B, 0x78, 0, x0 + 1, 20, x1 - 1, 29];
    else {
      const x = x0 + 1 + Math.trunc(((x1 - x0 - 1) - (b.length * 6 - 1)) / 2);
      out = [0x1B, 0x51, 21, 0x1B, 0x52, Math.max(x, x0 + 1), ...b, 0x1B, 0x51, 20];
    }
    this.inside = true;
    this.feed(out);
    this.inside = false;
    return true;
  }
  // The name again, small, on rows 10-16 - the match's boxes take 19-30.
  small() {
    this.big = false;
    const name = this.name.slice(0, 20);
    const out = [0x1B, 0x78, 0, 0, 7, this.w - 1, 21,
                 0x1B, 0x4B, 2, 0x1B, 0x51, 10, 0x1B, 0x52, Math.max(0, Math.trunc((this.w - name.length * 6) / 2))];
    for (const ch of name) out.push(ch.charCodeAt(0));
    out.push(0x1B, 0x4B, this.font, 0x1B, 0x51, this.cy, 0x1B, 0x52, this.cx);
    this.inside = true;
    this.feed(out);
    this.inside = false;
  }
  rankLine() {
    const r = this.ranks[this.name.toLowerCase()];
    if (!this.showRanks || !this.name || !r || r[0] < 1) return "";
    const title = r[1].slice(0, 12);
    let n = r[0] >= 4 ? Math.min(r[0] - 3, 8) : 0;      // one more each rank from Sergeant, as many as fit
    while (n && (title.length + 2 + 2 * n) * 6 > this.w) n--;
    const stars = "*".repeat(n);
    return stars ? `${stars} ${title} ${stars}` : title;
  }
  rank(force) {
    if (!this.showRanks) return;
    const line = this.rankLine();
    if (line === this.shown && (!force || !line)) return;
    const out = [0x1B, 0x78, 0, 0, 0, this.w - 1, 6];
    if (line) {
      out.push(0x1B, 0x4B, 0, 0x1B, 0x51, 0, 0x1B, 0x52, Math.max(0, Math.trunc((this.w - line.length * 6) / 2)));
      for (const ch of line) out.push(ch.charCodeAt(0));
      out.push(0x1B, 0x4B, this.font, 0x1B, 0x51, this.cy, 0x1B, 0x52, this.cx);
    }
    this.shown = line;
    this.inside = true;
    this.feed(out);                     // into the same stream, as the shim's goes
    this.inside = false;
  }
  draw(canvas) {
    const S = 6, w = this.w * S, h = PL_H * S;
    canvas.width = w; canvas.height = h;
    const g = canvas.getContext("2d");
    g.fillStyle = "#000"; g.fillRect(0, 0, w, h);
    const r = (S - 1) / 2;
    for (const on of [0, 1]) {
      g.fillStyle = on ? "#ff7a1a" : "#2a0e04";
      g.beginPath();
      for (let y = 0; y < PL_H; y++)
        for (let x = 0; x < this.w; x++)
          if (this.dots[y * this.w + x] === on) { g.moveTo(x * S + S / 2 + r, y * S + S / 2); g.arc(x * S + S / 2, y * S + S / 2, r, 0, 2 * Math.PI); }
      g.fill();
    }
  }
}

// What the game writes (0x569DE0): a string at (x, y), centred on (56, 15)
// when both are 0 - large if nine characters or fewer, else small, at most
// 20 - each command a write of its own.
function gameText(sign, str, x, y, font) {
  let len = str.length;
  if (!len) return;
  if (font !== 2) font = len <= 9 ? 5 : 2;
  len = Math.min(len, 20);
  if (!x && !y) {
    const [w, h] = font === 4 || font === 5 ? [10, 14] : [5, 7];
    x = (0x38 - ((w * len) >> 1)) & 0xFF;
    y = (0x0F - (h >> 1)) & 0xFF;
  }
  sign.feed([0x1B, 0x52, x]); sign.feed([0x1B, 0x51, y]); sign.feed([0x1B, 0x48, 0]); sign.feed([0x1B, 0x4B, font]);
  sign.feed([...str.slice(0, len)].map(c => c.charCodeAt(0) & 0xFF));
}

// The sign as the console seats a pilot (LOAD): cleared, then their name.
function gameName(sign, name) {
  sign.feed([0x1B, 0x40]);
  gameText(sign, "         ", 0, 0, 5);
  gameText(sign, name, 0, 0, 5);
}

// In a match (0x569EE0), every second or so: the boxes the first time, then
// the pilot's place and score whenever they change.
function gameMatch(sign, place, score, first) {
  if (first) {
    sign.feed([0x1B, 0x78, 0, 27, 19, 93, 30]);
    sign.feed([0x1B, 0x58, 27, 19, 42, 30]);
    sign.feed([0x1B, 0x58, 42, 19, 93, 30]);
  }
  const p = String(place), s = String(score);
  if (p.length < 1 || p.length > 2 || s.length < 1 || s.length > 8) return;
  gameText(sign, "  ", 29, 20, 2);
  gameText(sign, p, p.length === 1 ? 33 : 29, 20, 2);
  gameText(sign, "        ", 44, 20, 2);
  gameText(sign, s, ((17 - s.length) * 5) & 0xFF, 20, 2);
}

// ------------------------------------------------------------ the page
let PL = null;          // { ranks: the rank table, pilots: [{name, rank, title}] }

function plRanks() { return (PL && PL.ranks) || []; }
function plShowRanks() { return !V || V.plasma_ranks !== false; }
function plWidth() { return +(V && V.plasma_width) || 128; }

function signFor(name, rank, match) {
  const t = plRanks()[rank];
  const sign = new Sign(t ? { [name.toLowerCase()]: [rank, t.title.toUpperCase()] } : {}, plShowRanks(), plWidth());
  gameName(sign, name);
  if (match) gameMatch(sign, match.place, match.score, true);
  return sign;
}

function signCanvas(sign) {
  const c = document.createElement("canvas");
  c.className = "sign";
  sign.draw(c);
  return c;
}

function renderPlasma() {
  if (!PL) return;
  const name = ($("#pl-name").value || "").slice(0, 15) || "Odwalla";
  const grid = $("#pl-ranks");
  grid.innerHTML = "";
  plRanks().forEach((r, i) => {
    const d = document.createElement("div");
    d.className = "sign-cell";
    d.appendChild(signCanvas(signFor(name, i)));
    d.insertAdjacentHTML("beforeend", `<div class="sign-cap">${rankBadge({ rank: i, short: r.short, title: r.title }, true)} ${esc(r.title)}
      <span class="hint">${i === 0 ? "no matches yet: no rank line" : i >= 4 ? "veteran: " + stars(signFor(name, i).shown) : ""}</span></div>`);
    grid.appendChild(d);
  });
  // Try it: one pilot, seated or in a match.
  const pick = $("#pl-pilot").value, rank = +$("#pl-rank").value;
  const who = pick ? (PL.pilots.find(p => p.name === pick) || {}) : null;
  const tname = who ? who.name : (($("#pl-tname").value || "").slice(0, 15) || "Odwalla");
  const trank = who ? who.rank : rank;
  $("#pl-rank").disabled = !!who; $("#pl-tname").disabled = !!who;
  if (who) $("#pl-rank").value = String(trank);
  const state = $("#pl-state").value;
  const match = state === "match" ? { place: Math.max(1, Math.min(99, +$("#pl-place").value || 1)), score: Math.trunc(+$("#pl-score").value || 0) } : null;
  $$(".pl-match").forEach(e => e.hidden = !match);
  const sign = signFor(tname, trank, match);
  const box = $("#pl-try");
  box.innerHTML = "";
  box.appendChild(signCanvas(sign));
  PL_LAST = sign.bytes;
  $("#pl-led-pattern").innerHTML = "";
  $("#pl-led-pattern").appendChild(signCanvas(rawSign(wiringTest())));
  $("#pl-bytes").textContent = sign.bytes.map(b => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");
  $("#pl-line").textContent = sign.shown ? `Rank line: "${sign.shown}" - the shim's, on rows 0-6.` :
    plShowRanks() ? "No rank line: " + (trank ? "" : "a pilot with no matches is a Recruit, and Recruits have none.") : "No rank line: ranks are off.";
  $("#pl-long").innerHTML = "";
  for (const [n, r, m] of [["Longbow", 6, null], ["Christopher", 4, null], ["Odwalla", 8, { place: 1, score: 4250 }], ["Christopher", 2, { place: 3, score: 870 }]]) {
    const d = document.createElement("div");
    d.className = "sign-cell";
    d.appendChild(signCanvas(signFor(n, r, m)));
    d.insertAdjacentHTML("beforeend", `<div class="sign-cap">${esc(n)}, ${esc(plRanks()[r].title)} <span class="hint">${m ? "small in a match, above the boxes" : n.length > 9 ? "ten or more: small" : "nine letters or fewer: large"}${m ? `, in a match: ${ordinal(m.place)}, ${m.score} points` : ", seated"}</span></div>`);
    $("#pl-long").appendChild(d);
  }
}

// ------------------------------------------------------------ the LED sign
// A board on this PC (firmware/rp2040-plasma) takes the same bytes: Try it's
// sign as the game's, or a test pattern written in the sign's own dots.
let PL_LAST = null;     // the sign's bytes as Try it shows them

function rawSign(bytes) {
  const s = new Sign({}, false, plWidth());
  s.inside = true;                      // the sign's own x's: nothing moved, no rank line
  s.feed(bytes);
  s.inside = false;
  return s;
}

function wiringTest() {
  const w = plWidth(), out = [0x1B, 0x40, 0x1B, 0x58, 0, 0, w - 1, PL_H - 1, 0x1B, 0x4B, 0];
  const text = (s, x, y) => { out.push(0x1B, 0x52, x, 0x1B, 0x51, y); for (const c of s) out.push(c.charCodeAt(0)); };
  text("TL", 3, 3); text("TR", w - 15, 3); text("BL", 3, PL_H - 10); text("BR", w - 15, PL_H - 10);
  for (let x = 0; x < w; x++) {         // after the letters, whose cells would rub it out
    const y = Math.round(x * (PL_H - 1) / (w - 1));
    out.push(0x1B, 0x78, 1, x, y, x, y);
  }
  return out;
}

function ledState(r) {
  $("#pl-led").textContent = !r ? "" : !r.available ? "This launcher has no pyserial, which the LED sign needs."
    : r.boards.length ? `The sign's board is on ${r.boards.join(", ")}; the pod's game finds it by itself (shim.ini [plasma] device).`
    : "No LED sign on this PC - plug in its board (Firestorm Plasma, USB 1209:0004), then Find it.";
}

async function ledSend(bytes, doing) {
  status(doing, dirty);
  const r = await api("/api/plasma/send", { bytes });
  if (r.error) status("Error: " + r.error, true, true);
  else { status(r.message, dirty); ledState({ available: true, boards: r.boards }); }
}

async function ledFind() { ledState(await api("/api/plasma").catch(() => null)); }

$("#pl-led-find").addEventListener("click", ledFind);
$("#pl-led-try").addEventListener("click", () => PL_LAST && ledSend(PL_LAST, "Showing Try it on the LED sign..."));
$("#pl-led-test").addEventListener("click", () => ledSend(wiringTest(), "Showing the wiring test..."));
$("#pl-led-all").addEventListener("click", () => ledSend([0x1B, 0x40, 0x1B, 0x78, 1, 0, 0, plWidth() - 1, PL_H - 1], "Lighting every dot..."));
$("#pl-led-clear").addEventListener("click", () => ledSend([0x1B, 0x40], "Clearing the LED sign..."));

function stars(line) {
  const n = (line.match(/^\*+/) || [""])[0].length;
  return n + " star" + (n === 1 ? "" : "s");
}

function ordinal(n) {
  const t = ["th", "st", "nd", "rd"], v = n % 100;
  return n + (t[(v - 20) % 10] || t[v] || t[0]);
}

async function loadPlasma() {
  const lb = await api("/api/career").catch(() => null);
  const ranks = (lb && lb.ranks) || [];
  PL = { ranks, pilots: ((lb && lb.pilots) || []).filter(p => p.name).map(p => ({ name: p.name, rank: p.rank || 0, title: p.title }))
    .sort((a, b) => a.name.localeCompare(b.name)) };
  const keep = $("#pl-pilot").value;
  $("#pl-pilot").innerHTML = `<option value="">Any name, any rank</option>` +
    PL.pilots.map(p => `<option value="${esc(p.name)}">${esc(p.name)} - ${esc(p.title || "Recruit")}</option>`).join("");
  if (PL.pilots.some(p => p.name === keep)) $("#pl-pilot").value = keep;
  if (!$("#pl-rank").options.length && ranks.length) {
    $("#pl-rank").innerHTML = ranks.map(r => `<option value="${r.rank}">${esc(r.title)}</option>`).join("");
    $("#pl-rank").value = "4";          // Sergeant: the first with stars
  }
  renderPlasma();
  ledFind();
}

["#pl-name", "#pl-tname", "#pl-place", "#pl-score"].forEach(s => $(s).addEventListener("input", renderPlasma));
["#pl-pilot", "#pl-rank", "#pl-state"].forEach(s => $(s).addEventListener("change", renderPlasma));
$("#plasma_ranks").addEventListener("change", () => setTimeout(renderPlasma));
$("#plasma_width").addEventListener("change", () => setTimeout(renderPlasma));
window.addEventListener("hashchange", () => { if (location.hash === "#plasma") loadPlasma(); });
if (location.hash === "#plasma") loadPlasma();
