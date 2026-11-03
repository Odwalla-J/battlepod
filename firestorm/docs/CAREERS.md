# Pilot careers, ranks and leaderboards

Every match a pod flies is added to its pilot's career: matches, wins,
kills, deaths, score, best match, favourite 'Mech - and a rank, shown in the
launcher and on top of the pod, on the plasma sign.

## Where the numbers come from

As a match ends, the pod's `MW4.exe` writes `c:\mw4files\<guid>.pr` (under
the shim, `mw4files\<role>\`), the record VWE's `mw4print` turns into the
battle report. Nothing is written while the match runs; the file appears
whole at the end. Its layout, as far as careers need it (`launcher/career.py`):

| where | what |
|---|---|
| `0x00` | `68 A7 57 39`, the same in every record |
| `0x08` | a `SYSTEMTIME`: when the match started, local time |
| before `Resource\Missions\` | the mission (`Nazca`) and its display name with the game (`Nazca - Attrition`), each a length byte and the text |
| about `0x54A` | the players: a count (4 bytes), then for each: a byte (1 for this pod's own pilot, 3 for the others), a byte (1), 2 bytes of id, the name and the 'Mech's chassis (each 4 bytes of length and the text), 4 bytes (8) and 8 more, then score, kills and deaths (4 bytes each) |
| after | the match, event by event (the report's *Mission Highlights*) |

Checked on fifteen records against the reports printed from them: every
score, kill and death agrees. Each pod's record has everybody in it, so each
pod reports only its own pilot (the 1): no one is counted twice, and the
computer's pilots are never counted.

## The way they travel

- **A pod's launcher** (role *pod* or *both*) looks at its records every 15
  s and on start, and sends each finished one (unchanged for 15 s) to the
  console's launcher - `POST /pod/career` on its pod port, 8790. Ones it has
  sent are listed in `profiles\career-sent.txt`. Records from before careers
  existed, and from while the launcher was closed, go the next time it runs;
  if the console cannot be reached, they wait.
- **The console's launcher** keeps them in `profiles\career.json`, one line
  per pilot per match (the same match twice is ignored), tied to the pilot's
  profile by callsign. A pilot with no profile is kept by name and shown as a
  *guest*. Totals, ranks and standings are summed from those lines whenever
  asked, so they are never out of step, and a rank table changed later
  applies to every match ever flown.
- **Back to the pod:** every pilot's rank, as `profiles\ranks.ini`
  (`Odwalla=2,MECHWARRIOR,MW`), which the pod's shim reads for the plasma
  sign.

## Ranks

Experience: each match's score (a negative one counts as 0), plus 250 for
flying it and 500 for finishing first (by score, among everyone, the
computer's pilots too). A pilot with no matches is a Recruit.

| rank | short | experience |
|---|---|---|
| Recruit | RCT | no matches |
| Cadet | CDT | 1 |
| MechWarrior | MW | 2,000 |
| Corporal | CPL | 5,000 |
| Sergeant | SGT | 10,000 - a veteran from here |
| Lieutenant | LT | 17,500 |
| Captain | CPT | 27,500 |
| Major | MAJ | 40,000 |
| Colonel | COL | 60,000 |
| General | GEN | 90,000 |

The table is `RANKS` in `launcher/career.py`. In the matches flown so far a
pilot scores from 0 to about 6,000 a match against the computer, so Cadet
comes with the first match, MechWarrior after a few, Sergeant after a good
evening and General after a long season.

## In the launcher

- **Leaderboards** (its own page): the best of the period - highest rank,
  most kills, best match, kills per death (3 matches or more) - then every
  pilot's standing, by rank or by any column, for all time, this month, the
  last 7 days or today; the recent matches; the rank ladder.
- **Pilots:** each pilot's rank badge in the roster, and in the editor their
  rank, how far to the next, and their career in eight numbers.

On a pod, both pages are the console's, through its launcher.

## On the plasma sign

When the game writes a pilot's name on the sign and that pilot has a rank,
the shim writes the rank on the sign's top row (rows 0-6, which the game
never uses): `MECHWARRIOR`, and from Sergeant up between stars - one for a
Sergeant and one more each rank above, as many as the sign's width takes
(`[plasma] width`): on the centre's 128-dot LED matrices `* SERGEANT *` up
to `****** GENERAL ******`; on the game's own 112 dots, four at most - a
veteran, seen across the room. It goes into the
same byte stream as the game's own text (font, place, text, then the game's
font and place put back), so a real sign fed from the control port shows it
too. In a match, when the game puts its place and score boxes under the name
(rows 19-30, over the last rows of a large name), the shim writes the name
again small on rows 10-16, between the rank and the boxes; seated, it stays
as the game drew it. `[plasma] ranks = 0` leaves the sign as the game draws it (the
launcher's **Plasma sign** page has the switch).

The launcher's Plasma sign page shows the sign for every rank, and for any
name, rank, place and score: the bytes the game sends (`0x569DE0`-
`0x56A3ED`) put through the same interpreter, font and rank line as the
shim's, so it is what a pod shows, dot for dot.
