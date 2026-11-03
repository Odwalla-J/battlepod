#!/usr/bin/env python3
"""BattleTech 3.0, for the launcher: the 1990s pod (VWE release 13.1.8) on
battlepod's emulator.

A battle is battlepod's tools/hub.py - the centre: the operator console's
start of a game, the network between pods, and CPU pilots - with this PC's
cockpit (cockpit.exe, the pod's 68020 running its own firmware, drawing the
main view) as one of the pods, and as many CPU pilots as asked, each a whole
emulated pod of its own (battlepod.exe, no window).

Where things are:

    a packaged copy (tools/package.py):  <copy>\\battletech\\
        cockpit.exe, battlepod.exe, tools\\hub.py ..., Game Files\\, battletech.ini, logs\\
    the battlepod repository:            build\\cockpit.exe, build\\battlepod.exe,
        tools\\hub.py; the settings and logs in build\\

battletech.ini is the cockpit's (src/battlepod.c, "the cockpit's settings":
[view], [panels], [lamps], [displays], [bargraphs], [joy]) and this page's
([game]: the release's Game Files folder and the last battle's choices). The
hub hands its name to every pod in BATTLEPOD_INI.

The release itself is never copied by the repository: the Game Files folder
is the person's own (VWE Release 13.1.8, from the Internet Archive).
"""
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# The pod's Mechs as Vehicle_List numbers them, when the release's own list
# cannot be read. Drone and Director are not Mechs a pilot takes out.
NOT_MECHS = {24, 170}
ENVS = [(0, "Day"), (1, "Dusk"), (2, "Night")]


def find_home():
    """(home, bin, tools, writable folder) for this launcher, or None."""
    packaged = os.path.normpath(os.path.join(HERE, "..", "battletech"))
    if os.path.isfile(os.path.join(packaged, "cockpit.exe")) and os.path.isfile(os.path.join(packaged, "tools", "hub.py")):
        return packaged, packaged, os.path.join(packaged, "tools"), packaged
    repo = os.path.normpath(os.path.join(HERE, "..", ".."))
    if os.path.isfile(os.path.join(repo, "tools", "hub.py")):
        return repo, os.path.join(repo, "build"), os.path.join(repo, "tools"), os.path.join(repo, "build")
    return None


class Ini:
    """A small ini file kept as lines, comments and order left alone."""

    def __init__(self, path):
        self.path = path
        try:
            with open(path, encoding="utf-8-sig") as f:
                self.lines = f.read().splitlines()
        except OSError:
            self.lines = []

    def _find(self, sec):
        start = None
        for i, l in enumerate(self.lines):
            m = re.match(r"\s*\[(.+?)\]", l)
            if m:
                if start is not None:
                    return start, i
                if m.group(1).lower() == sec.lower():
                    start = i
        return (start, len(self.lines)) if start is not None else None

    def get(self, sec, key, default=None):
        r = self._find(sec)
        if not r:
            return default
        for l in self.lines[r[0] + 1:r[1]]:
            m = re.match(r"\s*([^=;#]+?)\s*=\s*(.*)$", l)
            if m and m.group(1).lower() == key.lower():
                return m.group(2).strip()
        return default

    def set(self, sec, key, value):
        r = self._find(sec)
        if not r:
            if value is None:
                return
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines += ["[%s]" % sec, "%s=%s" % (key, value)]
            return
        for i in range(r[0] + 1, r[1]):
            m = re.match(r"\s*([^=;#]+?)\s*=", self.lines[i])
            if m and m.group(1).lower() == key.lower():
                if value is None:
                    del self.lines[i]
                else:
                    self.lines[i] = "%s=%s" % (key, value)
                return
        if value is not None:
            at = r[1]
            while at > r[0] + 1 and not self.lines[at - 1].strip():
                at -= 1
            self.lines.insert(at, "%s=%s" % (key, value))

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8", newline="\r\n") as f:
            f.write("\n".join(self.lines).rstrip() + "\n")


# What the page edits, with the cockpit's defaults: (section, key, default).
SETTINGS = [
    ("view", "x", ""), ("view", "y", ""), ("view", "w", "960"), ("view", "h", "888"),
    ("view", "fullscreen", "0"), ("view", "monitor", "1"), ("view", "smooth", "1"), ("view", "strip", "1"),
    ("view", "effects", "1"),
    ("panels", "show", "0"),
    ("radar", "show", "1"), ("radar", "x", ""), ("radar", "y", ""), ("radar", "w", "960"), ("radar", "h", "600"),
    ("radar", "fullscreen", "0"), ("radar", "monitor", "2"), ("radar", "page", "0"), ("radar", "range", "1"),
    ("radar", "layout", ""),
    ("lamps", "x", ""), ("lamps", "y", ""), ("displays", "x", ""), ("displays", "y", ""),
    ("bargraphs", "x", ""), ("bargraphs", "y", ""),
    ("joy", "enabled", "1"), ("joy", "deadzone", "4"),
    ("joy", "throttle", "Firestorm Throttle.X"), ("joy", "stick_x", "Firestorm Stick.X"),
    ("joy", "fire1", "Firestorm Stick.B1"), ("joy", "fire2", "Firestorm Stick.B2"),
    ("joy", "fire3", "Firestorm Stick.B3"), ("joy", "fire_all", ""),
    ("joy", "target", "Firestorm Stick.B4"), ("joy", "light", "Firestorm Throttle.B2"),
    ("joy", "twist_left", "Firestorm Stick.HatLeft"), ("joy", "twist_right", "Firestorm Stick.HatRight"),
    ("joy", "centre", "Firestorm Throttle.B1"), ("joy", "view_up", "Firestorm Stick.HatUp"),
    ("joy", "view_down", "Firestorm Stick.HatDown"), ("joy", "zoom_in", ""), ("joy", "zoom_out", ""),
]
BATTLE = [("scenario", "BadLands-16"), ("vehicle", "0"), ("pilot", "Pilot"), ("paint", "0"), ("accent", "0"),
          ("bots", "2"),
          ("opponents", "8,12,30,34,4,26,13"), ("env", "0"), ("arena", "1"), ("minutes", "0"),
          ("remote", "0"), ("remote_names", "Guest2,Guest3,Guest4"), ("remote_vehicles", "8,12,30"),
          ("join_host", ""), ("join_seat", "2"), ("respawn", "10"), ("local", "1")]
HUB_PORT = 5170


class BT3:
    def __init__(self, project=None):
        self.home = find_home()
        self.project = project          # where to look for a release when none is set
        self.proc = None                # the battle this PC runs (the hub)
        self.joined = None              # this PC's cockpit in another PC's battle
        self.seats = []                 # the centre's pods' seats in the battle this PC runs

    # ---- where things are
    @property
    def available(self):
        return self.home is not None

    def ini_path(self):
        return os.path.join(self.home[3], "battletech.ini")

    def ini(self):
        return Ini(self.ini_path())

    def logs(self):
        return os.path.join(self.home[3], "logs")

    def state_path(self):
        return os.path.join(self.logs(), "battle.json")

    @staticmethod
    def is_game_files(p):
        return bool(p) and os.path.isfile(os.path.join(p, "Full_Load_3_0")) and os.path.isdir(os.path.join(p, "Scenarios"))

    def game_files(self):
        """The release's Game Files folder: the one set, the packaged copy's,
        or one found near this launcher."""
        p = self.ini().get("game", "files", "")
        if self.is_game_files(p):
            return p
        if self.home:
            p = os.path.join(self.home[0], "Game Files")
            if self.is_game_files(p):
                return p
        p = self.find_game_files()
        if p and self.home:                     # found once, kept: no search each time
            ini = self.ini()
            ini.set("game", "files", p)
            ini.save()
        return p or ""

    def find_game_files(self):
        roots = []
        if self.home:
            # The repository's Play.cmd names the release it was set up with.
            play = os.path.join(self.home[0], "Play.cmd")
            try:
                m = re.search(r'VWE_GAME_FILES=\\?"([^"\\]+)', open(play, encoding="latin-1").read())
                if m:
                    p = m.group(1)
                    if re.match(r"^/[a-z]/", p):
                        p = p[1].upper() + ":" + p[2:]
                    p = p.replace("/", "\\")
                    if self.is_game_files(p):
                        return p
            except OSError:
                pass
            roots.append(os.path.dirname(self.home[0]))
        if self.project:
            roots.append(self.project)
        for root in roots:
            for dirpath, dirs, files in os.walk(root):
                if dirpath[len(root):].count(os.sep) > 9:
                    dirs[:] = []
                    continue
                dirs[:] = [d for d in dirs if d not in ("build", "third_party", ".git", "node_modules", "MW4", "game")]
                if "Full_Load_3_0" in files and self.is_game_files(dirpath):
                    return dirpath
        return None

    # ---- what the release offers
    def scenarios(self, gf):
        try:
            names = sorted(n for n in os.listdir(os.path.join(gf, "Scenarios")) if not n.endswith(".rsrc"))
        except OSError:
            return []
        out = []
        for n in names:
            m = re.match(r"^(.*?)-(\d+)$", n)
            label = (m.group(1) if m else n).replace("_", " ")
            out.append({"file": n, "name": label, "pods": int(m.group(2)) if m else None})
        return out

    def vehicles(self, gf):
        out = []
        try:
            text = open(os.path.join(gf, "Vehicle_List"), "rb").read().decode("mac-roman", "replace")
        except OSError:
            return out
        for line in text.replace("\r", "\n").split("\n"):
            m = re.match(r"^\s*(\d+)\s+(.+?)\s*$", line)
            if not m or int(m.group(1)) in NOT_MECHS:
                continue
            n, label = int(m.group(1)), m.group(2)
            code, _, name = label.partition("-")
            chassis = re.sub(r"\s+(Prime|V\d+)$", "", name or label)
            out.append({"id": n, "code": code if name else "", "name": name or label, "chassis": chassis})
        return out

    # ---- settings
    def settings(self):
        ini = self.ini()
        s = {"%s.%s" % (sec, key): ini.get(sec, key, d) for sec, key, d in SETTINGS}
        s.update({"game.%s" % k: ini.get("game", k, d) for k, d in BATTLE})
        return s

    def save(self, body):
        ini = self.ini()
        known = {"%s.%s" % (sec, key) for sec, key, _ in SETTINGS} | {"game.%s" % k for k, _ in BATTLE} | {"game.files"}
        for k, v in body.items():
            if k not in known:
                continue
            sec, key = k.split(".", 1)
            v = ("%s" % v).strip() if v is not None else ""
            if k == "game.files" and v and not self.is_game_files(v):
                raise ValueError("%s is not the release's Game Files folder (it has no Full_Load_3_0 and Scenarios)" % v)
            if k == "game.pilot":
                v = re.sub(r"[^ -~]", "", v)[:8]
            if k == "game.remote_names":
                v = ",".join(re.sub(r"[^ -~]", "", n).replace(",", "")[:8] for n in v.split(","))
            ini.set(sec, key, v if v != "" else None)
        # The cockpit names its window; keep it ours.
        ini.set("view", "title", "BattleTech 3.0")
        ini.save()

    # ---- running
    def running(self):
        """A battle on: the hub this launcher started, or - one started
        before it opened - a battle file still being written."""
        if self.proc and self.proc.poll() is None:
            return True
        try:
            fresh = time.time() - os.path.getmtime(self.state_path()) < 4
        except OSError:
            return False
        b = self.battle()
        return fresh and bool(b) and not b.get("over")

    def pids(self):
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='cockpit.exe' or Name='battlepod.exe' or "
              "((Name='python.exe' or Name='pythonw.exe') and CommandLine like '%hub.py%')\" | "
              "Select-Object ProcessId,Name | ConvertTo-Json -Compress")
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                                 timeout=15, creationflags=0x08000000).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            return []
        if not out:
            return []
        d = json.loads(out)
        return [p["ProcessId"] for p in (d if isinstance(d, list) else [d])]

    def start(self, body, human_args="", pods=None):
        """The battle, on this PC's hub. pods, on a centre's console, is the
        pods in service - each a person's seat ({"name", ...}), taking the
        place of the "other PCs" setting; the caller tells them to join
        (self.seats: their seat numbers, in that order). game.local = 0
        leaves this PC without a cockpit of its own: the operator's."""
        if not self.available:
            raise ValueError("battlepod's emulator is not with this launcher")
        for f in ("cockpit.exe", "battlepod.exe"):
            if not os.path.isfile(os.path.join(self.home[1], f)):
                raise ValueError("%s is not built (in the battlepod repository: make && make cockpit)" % f)
        if body:
            self.save(body)
        # What the cockpit is not told it takes as its own default; tell it
        # the launcher's (the strip, the radar window).
        ini = self.ini()
        for sec, key, d in SETTINGS:
            if d != "" and ini.get(sec, key) is None:
                ini.set(sec, key, d)
        ini.save()
        gf = self.game_files()
        if not self.is_game_files(gf):
            raise ValueError("where is the release? Set its Game Files folder on the BattleTech 3.0 page")
        s = self.settings()
        self.stop()
        os.makedirs(self.logs(), exist_ok=True)
        try:
            os.remove(self.state_path())
        except OSError:
            pass
        local = s["game.local"] != "0" or pods is None
        remote = max(0, min(3, int(s["game.remote"] or 0))) if pods is None else min(len(pods), 8 - local)
        bots = max(0, min(8 - local - remote, int(s["game.bots"] or 0)))
        if not local and not remote:
            raise ValueError("no pod is in service, and this PC has no cockpit: put a pod in service (Pods page)")
        opp = [v for v in (s["game.opponents"] or "").split(",") if v.strip().isdigit()]
        rv = [v for v in (s["game.remote_vehicles"] or "").split(",") if v.strip().isdigit()]
        rn = [n for n in (s["game.remote_names"] or "").split(",")]
        if pods is not None:            # the operator's seat list is for the pods it names, in order
            rn = [re.sub(r"[^ -~]", "", p.get("pilot") or p.get("name") or "").replace(",", "")[:8] for p in pods]
        vehicles = [str(int(s["game.vehicle"] or 0))] + (rv + ["8", "12", "30", "34", "4", "26", "13"])[:remote] + (opp + ["8"] * 8)[:bots]
        names = ([s["game.pilot"] or "Pilot"] + [(rn[i] if i < len(rn) and rn[i] else "Guest%d" % (i + 2))
                                                 for i in range(remote)] + BOT_NAMES[:bots])
        # The person's paint, then each other Mech in desert with an accent
        # of its own - the next free one after theirs (hub.py, --paint).
        accent = max(0, min(7, int(s["game.accent"] or 0)))
        paint = [str(max(0, min(5, int(s["game.paint"] or 0))) * 10 + accent)]
        paint += [str((accent + 1 + i) % 8) for i in range(remote + bots)]
        if not local:                   # the first entries were this PC's cockpit's
            vehicles, names, paint = vehicles[1:], names[1:], paint[1:]
        self.seats = [i + 1 + local for i in range(remote)]
        argv = [sys.executable, os.path.join(self.home[2], "hub.py")] + (["--human"] if local else []) + [
                "--bots", str(bots),
                "--scenario", s["game.scenario"] or "BadLands-16", "--env", str(int(s["game.env"] or 0)),
                "--vehicles", ",".join(vehicles), "--names", ",".join(names), "--paint", ",".join(paint),
                "--state", self.state_path(), "--bin", self.home[1], "--logs", self.logs()]
        if s["game.arena"] == "1":
            argv.append("--arena")
        if float(s["game.respawn"] or 0) > 0:
            argv += ["--respawn", str(float(s["game.respawn"]))]
        if human_args:
            argv += ["--human-args", human_args]
        if remote:
            argv += ["--remote", str(remote), "--listen", "0.0.0.0", "--port", str(HUB_PORT)]
        if pods is not None:
            argv.append("--end-remote")
        minutes = float(s["game.minutes"] or 0)
        if minutes > 0:
            argv += ["--seconds", str(int(minutes * 60) + 20)]     # the drop takes a little
        env = dict(os.environ, VWE_GAME_FILES=gf, BATTLEPOD_INI=self.ini_path(), PYTHONUNBUFFERED="1")
        log = open(os.path.join(self.logs(), "hub.log"), "w")
        self.proc = subprocess.Popen(argv, cwd=self.home[0], env=env, stdout=log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, creationflags=0x08000000 | 0x00000200)
        log.close()
        if not local:
            return "Starting the battle on %s: %s in the pods, %d CPU pilot%s." % (
                s["game.scenario"], ", ".join(names[:remote]), bots, "" if bots == 1 else "s")
        msg = ("Starting %s on %s against %d CPU pilot%s: the cockpit opens in a few seconds, and the Mech drops in "
               "(click its window if the keys do nothing)." % (names[0], s["game.scenario"], bots, "" if bots == 1 else "s"))
        if pods:
            msg += " The pods join in seats %s." % ", ".join(map(str, self.seats))
        elif remote:
            msg += " Seats %s wait for the other PCs: Join, this PC's address, that seat." % ", ".join(
                str(n) for n in range(2, 2 + remote))
        return msg

    def join(self, body):
        """This PC's cockpit in a battle another PC's launcher runs: seat N
        (2 and up) of its hub, the address this PC's launcher was given."""
        if not self.available:
            raise ValueError("battlepod's emulator is not with this launcher")
        if body:
            self.save(body)
        s = self.settings()
        host, seat = (s["game.join_host"] or "").strip(), int(s["game.join_seat"] or 2)
        if not re.match(r"^[0-9A-Za-z.-]+$", host):
            raise ValueError("which PC runs the battle? Its address, as its launcher's This PC page shows it")
        if not 1 <= seat <= 8:
            raise ValueError("a seat from 1 to 8")
        gf = self.game_files()
        if not self.is_game_files(gf):
            raise ValueError("where is the release? Set its Game Files folder on this page")
        sys.path.insert(0, self.home[2])
        import hub
        if self.joined and self.joined.poll() is None:       # this PC's cockpit, joined before
            self.joined.kill()
        os.makedirs(self.logs(), exist_ok=True)
        argv = [os.path.join(self.home[1], "cockpit.exe"), os.path.join(gf, "Full_Load_3_0")] + hub.POD_ARGS + [
            "--net", "%s:%d" % (host, HUB_PORT), "--net-node", str(seat), "--live-pod"]
        env = dict(os.environ, BATTLEPOD_INI=self.ini_path())
        log = open(os.path.join(self.logs(), "join.log"), "w")
        self.joined = subprocess.Popen(argv, cwd=self.home[0], env=env, stdout=log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, creationflags=0x08000000 | 0x00000200)
        log.close()
        return ("Joining the battle on %s in seat %d: the cockpit opens, and the Mech drops in once that PC's "
                "battle has started (its operator gave this seat its name and Mech)." % (host, seat))

    def stop(self):
        if self.joined and self.joined.poll() is None:
            self.joined.kill()
        pids = self.pids()
        if pids:
            subprocess.run(["taskkill", "/F"] + sum([["/PID", str(p)] for p in pids], []),
                           capture_output=True, creationflags=0x08000000)
            time.sleep(0.5)
        self.proc = None

    def stop_joined(self):
        """Only the cockpits that joined another PC's battle - not a battle
        this PC runs (whose cockpits reach its hub on 127.0.0.1)."""
        if self.joined and self.joined.poll() is None:
            self.joined.kill()
        self.joined = None
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='cockpit.exe'\" | Where-Object { $_.CommandLine -match "
              "'--net ' -and $_.CommandLine -notmatch '--net 127[.]' } | Select-Object -ExpandProperty ProcessId")
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                                 timeout=15, creationflags=0x08000000).stdout.split()
        except (OSError, subprocess.TimeoutExpired):
            out = []
        if out:
            subprocess.run(["taskkill", "/F"] + sum([["/PID", p] for p in out], []),
                           capture_output=True, creationflags=0x08000000)

    def battle(self):
        try:
            with open(self.state_path(), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def career_entries(self):
        """The finished battle, as a line for the careers (career.py) for
        each person in it - this PC's and the other PCs' - or none: none
        yet, still on, or shorter than a minute (a start abandoned). Score
        is 500 a kill, as Firestorm counts a destroyed 'Mech; place is by
        kills."""
        b = self.battle()
        if not b or not b.get("over") or not b.get("id") or b.get("t", 0) < 60:
            return []
        pods = b.get("pods") or []
        names = {v["id"]: v["name"] for v in self.vehicles(self.game_files())} if self.is_game_files(self.game_files()) else {}
        scen = re.sub(r"-\d+$", "", b.get("scenario") or "")
        out = []
        for me in pods:
            if not me.get("human") or (me.get("remote") and not me.get("joined")):
                continue
            place = 1 + sum(1 for p in pods if p["kills"] > me["kills"])
            out.append({"match": b["id"], "time": b.get("started") or time.strftime("%Y-%m-%d %H:%M"),
                        "mission": scen, "shown": scen + " - BattleTech 3.0", "game": GAME,
                        "name": me["name"], "mech": names.get(me["vehicle"], "Mech %s" % me["vehicle"]),
                        "score": 500 * me["kills"], "kills": me["kills"], "deaths": me["deaths"],
                        "place": place if me["kills"] else len(pods), "of": len(pods),
                        "players": [{"name": p["name"], "mech": names.get(p["vehicle"], ""), "score": 500 * p["kills"],
                                     "kills": p["kills"], "deaths": p["deaths"]} for p in pods]})
        return out

    def log_tail(self, n=14):
        try:
            with open(os.path.join(self.logs(), "hub.log"), encoding="utf-8", errors="replace") as f:
                # Not the centre's two-second position rows: the page draws those.
                lines = [l.rstrip() for l in f if not re.match(r"^t\s+\d+\s+relayed", l)]
                return lines[-n:]
        except OSError:
            return []

    def view(self):
        """Everything the page shows."""
        if not self.available:
            return {"available": False}
        gf = self.game_files()
        ok = self.is_game_files(gf)
        return {"available": True, "home": self.home[0], "ini": self.ini_path(), "game_files": gf, "files_ok": ok,
                "built": all(os.path.isfile(os.path.join(self.home[1], f)) for f in ("cockpit.exe", "battlepod.exe")),
                "scenarios": self.scenarios(gf) if ok else [], "vehicles": self.vehicles(gf) if ok else [],
                "envs": [{"id": i, "name": n} for i, n in ENVS], "settings": self.settings(),
                "running": self.running(), "battle": self.battle(), "log": self.log_tail()}


# What a 3.0 battle is called in the careers.
GAME = "BattleTech 3.0"

# CPU pilots' callsigns, eight characters at most (the pod's name field).
BOT_NAMES = ["Wolf", "Raven", "Viper", "Hammer", "Ghost", "Talon", "Striker", "Nova"]
