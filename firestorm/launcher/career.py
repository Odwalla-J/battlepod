#!/usr/bin/env python3
"""Pilot careers, for the launcher: each match's record read, kept on the
console, and summed into career stats, ranks and leaderboards.

The record. As a match goes, the pod's MW4.exe writes c:\\mw4files\\<guid>.pr
(the shim's mw4files\\<role>\\), the file VWE's mw4print reads for the
battle report. After a small bit-packed header it holds:

    0x00  4 bytes   68 A7 57 39            (the same in every record)
    0x08  SYSTEMTIME                       when the match started, local time
    ...   the mission, its display name and the game, as strings of a length
          byte and the text, just before "Resource\\Missions\\"
    ~0x54A the players: a count (4 bytes), then for each
            1 byte   1 for this pod's own pilot, 3 for everyone else
            1 byte   1
            2 bytes  an id
            4 bytes + text   the pilot's name
            4 bytes + text   the 'Mech's chassis ("Mad Cat")
            4 bytes  8, then 8 bytes (-1s for the others)
            3 x 4 bytes      score, kills, deaths
    ...   then the match, event by event (what the report tells blow by blow)

Read from fifteen records against the reports printed from them: every
score, kill and death agrees. Each pod writes one, with everybody in it, so
a pod reports only its own pilot - the console never counts anyone twice -
and the computer's pilots (never "1") are never counted at all.

The careers live in profiles\\career.json on the console: one line per pilot
per match. Everything else - totals, ranks, the leaderboards - is summed
from those when asked, so a rank table changed later applies to every match
ever flown.

usage: career.py RECORD.pr...    (prints what each record says)
       career.py --selftest
"""
import json
import os
import re
import struct
import sys
import threading
import time

MAGIC = b"\x68\xa7\x57\x39"

# Rank by experience: each match's score (a negative one counts as 0), plus
# MATCH_XP for flying it and WIN_XP for finishing first. A pilot with no
# matches is a Recruit. (title, short title, experience needed)
RANKS = [
    ("Recruit", "RCT", 0),
    ("Cadet", "CDT", 1),
    ("MechWarrior", "MW", 2000),
    ("Corporal", "CPL", 5000),
    ("Sergeant", "SGT", 10000),
    ("Lieutenant", "LT", 17500),
    ("Captain", "CPT", 27500),
    ("Major", "MAJ", 40000),
    ("Colonel", "COL", 60000),
    ("General", "GEN", 90000),
]
BT3_GAME = "BattleTech 3.0"     # bt3.py's battles: their "game"
MATCH_XP = 250
WIN_XP = 500
VETERAN = 4             # the rank (Sergeant) from which a pilot counts as a veteran


# ------------------------------------------------------------ the record

def _lstr(d, o, top):
    n = struct.unpack_from("<I", d, o)[0]
    if n > top or o + 4 + n > len(d):
        raise ValueError
    s = d[o + 4:o + 4 + n]
    if any(c < 0x20 or c > 0x7E for c in s):
        raise ValueError
    return s.decode("latin-1"), o + 4 + n


def _players(d, o):
    count = struct.unpack_from("<I", d, o)[0]
    if not 1 <= count <= 16:
        raise ValueError
    o += 4
    out = []
    for _ in range(count):
        kind, one = struct.unpack_from("<BB", d, o)
        if kind not in (1, 2, 3) or one != 1:
            raise ValueError
        o += 4
        name, o = _lstr(d, o, 31)
        if not name.strip():
            raise ValueError
        mech, o = _lstr(d, o, 40)
        if struct.unpack_from("<I", d, o)[0] != 8:
            raise ValueError
        o += 12
        score, kills, deaths = struct.unpack_from("<iii", d, o)
        if not (-100000 < score < 1000000 and 0 <= kills < 1000 and 0 <= deaths < 1000):
            raise ValueError
        o += 12
        out.append({"name": name.strip(), "mech": mech, "score": score, "kills": kills, "deaths": deaths,
                    "local": kind == 1})
    return out


def _mission(d):
    """(mission, display name, game) from the strings before Resource\\Missions\\."""
    i = d.find(b"Resource\\Missions\\")
    if i < 2:
        return "", "", ""
    strings, end = [], i - 1            # the length byte of "Resource\Missions\"
    for _ in range(2):
        for j in range(end - 1, max(end - 80, 0), -1):
            if d[j] == end - j - 1 and all(0x20 <= c <= 0x7E for c in d[j + 1:end]):
                strings.insert(0, d[j + 1:end].decode("latin-1"))
                end = j
                break
        else:
            break
    mission = strings[0] if len(strings) == 2 else ""
    shown = strings[-1] if strings else ""
    game = shown.split(" - ", 1)[1] if " - " in shown else ""
    return mission, shown, game


def read_record(path):
    """What a match record says, or None if it is not one (or not finished)."""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 0x600 or d[:4] != MAGIC:
        return None
    y, mo, _, day, h, mi, s, _ = struct.unpack_from("<8H", d, 8)
    if not (2000 <= y <= 2100 and 1 <= mo <= 12 and 1 <= day <= 31):
        return None
    players = None
    for o in range(0x10, min(len(d) - 40, 0x4000)):
        if d[o + 1:o + 4] != b"\0\0\0" or not 1 <= d[o] <= 16:
            continue
        try:
            players = _players(d, o)
            break
        except (ValueError, struct.error):
            pass
    if not players:
        return None
    mission, shown, game = _mission(d)
    return {"id": os.path.splitext(os.path.basename(path))[0], "time": "%04d-%02d-%02d %02d:%02d" % (y, mo, day, h, mi),
            "mission": mission, "shown": shown, "game": game, "players": players}


def entry_of(rec):
    """The line this pod reports: its own pilot in this match, with where they
    finished - by score, among everyone, the computer's pilots too."""
    me = next((p for p in rec["players"] if p["local"]), None)
    if not me:
        return None
    place = 1 + sum(1 for p in rec["players"] if p["score"] > me["score"])
    return {"match": rec["id"], "time": rec["time"], "mission": rec["mission"], "shown": rec["shown"],
            "game": rec["game"], "name": me["name"], "mech": me["mech"], "score": me["score"],
            "kills": me["kills"], "deaths": me["deaths"], "place": place, "of": len(rec["players"]),
            "players": [{k: p[k] for k in ("name", "mech", "score", "kills", "deaths")} for p in rec["players"]]}


# ------------------------------------------------------------ ranks

def xp_of(score, won):
    return max(score, 0) + MATCH_XP + (WIN_XP if won else 0)


def rank_of(xp, matches):
    i = 0
    if matches:
        for n, (_, _, need) in enumerate(RANKS):
            if xp >= need:
                i = n
    title, short, need = RANKS[i]
    nxt = RANKS[i + 1] if i + 1 < len(RANKS) else None
    return {"rank": i, "title": title, "short": short, "veteran": i >= VETERAN,
            "next": nxt[0] if nxt else None, "next_xp": nxt[2] if nxt else None,
            "progress": 1.0 if not nxt else max(0.0, min(1.0, (xp - need) / float(nxt[2] - need)))}


# ------------------------------------------------------------ the console's book

class Careers:
    """profiles\\career.json: every pilot's every match."""

    def __init__(self, game):
        self.path = os.path.join(game, "profiles", "career.json")
        self.lock = threading.Lock()

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
            return d if isinstance(d, dict) and isinstance(d.get("matches"), list) else {"matches": []}
        except (OSError, ValueError):
            return {"matches": []}

    def _save(self, d):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=0)
        os.replace(tmp, self.path)

    def add(self, e, pod, roster):
        """A pod's line for a match; the same match again is ignored."""
        for k in ("match", "time", "name"):
            if not isinstance(e.get(k), str) or not e[k]:
                raise ValueError("a match line needs %s" % k)
        if not re.match(r"^[0-9A-Za-z-]{8,64}$", e["match"]):
            raise ValueError("not a match id")
        line = {k: e.get(k) for k in ("match", "time", "mission", "shown", "game", "name", "mech")}
        for k in ("score", "kills", "deaths", "place", "of"):
            line[k] = int(e.get(k) or 0)
        line["players"] = [{k: p.get(k) for k in ("name", "mech", "score", "kills", "deaths")}
                           for p in (e.get("players") or [])[:16] if isinstance(p, dict)]
        line["pod"] = pod
        prof = next((p for p in roster if p["name"].lower() == line["name"].lower()), None)
        if not prof and len(line["name"]) == 8:     # 3.0's name plate: the callsign's first 8
            cut = [p for p in roster if p["name"][:8].lower() == line["name"].lower()]
            prof = cut[0] if len(cut) == 1 else None
        line["profile"] = prof["id"] if prof else None
        with self.lock:
            d = self._load()
            if any(m["match"] == line["match"] and m["name"].lower() == line["name"].lower() for m in d["matches"]):
                return False
            d["matches"].append(line)
            self._save(d)
            return True

    def matches(self):
        with self.lock:
            return self._load()["matches"]

    def summary(self, roster, since=None, game=None):
        """Each pilot's career (from `since`, "YYYY-MM-DD", if given; of one
        game, "fs" or "bt3", if given): by profile where the line has one,
        else by name. Ranks are always on the whole career, both games."""
        names = {p["id"]: p for p in roster}
        all_lines = self.matches()
        out = {}
        for m in all_lines:
            key = m.get("profile") if m.get("profile") in names else "name:" + m["name"].lower()
            c = out.setdefault(key, {"key": key, "profile": key if key in names else None, "name": m["name"],
                                     "matches": 0, "wins": 0, "kills": 0, "deaths": 0, "score": 0, "best": None,
                                     "xp": 0, "career_matches": 0, "mechs": {}, "last": "", "first": m["time"]})
            won = m.get("place") == 1 and (m.get("of") or 1) > 1
            c["xp"] += xp_of(m.get("score", 0), won)
            c["career_matches"] += 1
            if since and m["time"][:10] < since:
                continue
            if game and (m.get("game") == BT3_GAME) != (game == "bt3"):
                continue
            c["matches"] += 1
            c["wins"] += 1 if won else 0
            c["kills"] += m.get("kills", 0)
            c["deaths"] += m.get("deaths", 0)
            c["score"] += m.get("score", 0)
            c["best"] = m.get("score", 0) if c["best"] is None else max(c["best"], m.get("score", 0))
            mech = m.get("mech") or "?"
            c["mechs"][mech] = c["mechs"].get(mech, 0) + 1
            c["last"] = max(c["last"], m["time"])
            c["first"] = min(c["first"], m["time"])
        res = []
        for c in out.values():
            if c["profile"]:
                c["name"] = names[c["profile"]]["name"]
            c.update(rank_of(c["xp"], c["career_matches"]))
            c["avg"] = round(c["score"] / c["matches"]) if c["matches"] else 0
            c["kd"] = round(c["kills"] / c["deaths"], 2) if c["deaths"] else float(c["kills"])
            c["mech"] = max(c["mechs"].items(), key=lambda kv: kv[1])[0] if c["mechs"] else ""
            del c["mechs"]
            res.append(c)
        # Pilots on the roster who have not flown yet: Recruits.
        for pid, p in names.items():
            if pid not in out:
                c = {"key": pid, "profile": pid, "name": p["name"], "matches": 0, "wins": 0, "kills": 0,
                     "deaths": 0, "score": 0, "best": None, "xp": 0, "career_matches": 0, "last": "", "first": "",
                     "avg": 0, "kd": 0.0, "mech": ""}
                c.update(rank_of(0, 0))
                res.append(c)
        return res

    def recent(self, n=25, game=None):
        ms = [m for m in self.matches() if not game or (m.get("game") == BT3_GAME) == (game == "bt3")]
        return sorted(ms, key=lambda m: m["time"], reverse=True)[:n]

    def ranks_ini(self, roster):
        """profiles\\ranks.ini's text: each pilot's rank, for the plasma sign."""
        lines = ["; Each pilot's rank, for the pod's plasma sign: written by the launcher",
                 "; from the console's careers (profiles\\career.json). Not to be edited.", "[ranks]"]
        for c in sorted(self.summary(roster), key=lambda c: c["name"].lower()):
            if c["career_matches"] and "=" not in c["name"] and ";" not in c["name"][:1]:
                lines.append("%s=%d,%s,%s" % (c["name"], c["rank"], c["title"].upper(), c["short"]))
        return "\r\n".join(lines) + "\r\n"


def rank_table():
    return [{"rank": i, "title": t, "short": s, "xp": x} for i, (t, s, x) in enumerate(RANKS)]


def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text.replace("\r\n", "\n"):
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return True


def selftest():
    """A made record read back; a book with both games summed and filtered."""
    import struct as st
    import tempfile

    def lstr(s):
        return st.pack("<I", len(s)) + s.encode()

    def player(kind, name, mech, score, kills, deaths):
        return (bytes([kind, 1]) + st.pack("<H", 7) + lstr(name) + lstr(mech) + st.pack("<I", 8) +
                (b"\x01" * 8 if kind == 1 else b"\xff" * 8) + st.pack("<iii", score, kills, deaths))
    rec = bytearray(MAGIC + b"\x01\x00\x00\x00" + st.pack("<8H", 2026, 10, 4, 29, 16, 38, 49, 0))
    rec += b"\xff" * (0x1F4 - len(rec))
    rec += b"\x01\x05Nazca" + bytes([17]) + b"Nazca - Attrition" + bytes([18]) + b"Resource\\Missions\\"
    rec += b"\x00" * (0x54A - len(rec))
    rec += st.pack("<I", 3) + player(1, "Odwalla", "Dragon", 259, 0, 1) + player(3, "PAINGOD", "Mad Cat", 839, 1, 0) + \
        player(3, "Joker", "Mad Cat", 81, 0, 0)
    rec += b"\x00" * 0x400
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, "0123abcd-0000-0000-0000-000000000000.pr")
        with open(f, "wb") as out:
            out.write(rec)
        r = read_record(f)
        assert r and r["time"] == "2026-10-29 16:38" and r["mission"] == "Nazca" and r["game"] == "Attrition", r
        e = entry_of(r)
        assert (e["name"], e["mech"], e["score"], e["deaths"], e["place"], e["of"]) == ("Odwalla", "Dragon", 259, 1, 2, 3), e
        os.makedirs(os.path.join(tmp, "profiles"))
        book = Careers(tmp)
        roster = [{"id": "p1", "name": "Odwalla"}, {"id": "p2", "name": "LongCallsign"}]
        assert book.add(e, "pod1", roster) and not book.add(e, "pod1", roster)     # once
        bt = dict(e, match="bt3-00000001", game=BT3_GAME, name="LongCall", score=1000, kills=2, deaths=0, place=1, of=2)
        assert book.add(bt, "pod1", roster)
        s = {c["name"]: c for c in book.summary(roster)}
        assert s["Odwalla"]["matches"] == 1 and s["LongCallsign"]["matches"] == 1, s     # 8 characters, matched
        assert s["LongCallsign"]["wins"] == 1 and s["LongCallsign"]["title"] == "Cadet", s["LongCallsign"]
        only3 = {c["name"]: c["matches"] for c in book.summary(roster, game="bt3")}
        assert only3 == {"Odwalla": 0, "LongCallsign": 1}, only3
        assert [m["match"] for m in book.recent(game="fs")] == [e["match"]]
        assert "Odwalla=1,CADET,CDT" in book.ranks_ini(roster)
    assert rank_of(0, 0)["title"] == "Recruit" and rank_of(10000, 5)["title"] == "Sergeant"
    print("career selftest: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
        sys.exit(0)
    for p in sys.argv[1:]:
        r = read_record(p)
        print(p, json.dumps(entry_of(r) if r else None, indent=1))
