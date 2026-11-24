"""Race performance test: ASTRO on vs off, from the launcher's own frame counter.

The launcher writes "Perf: frames N ms T worst W ..." every 5 seconds to last_run_<name>.log and overwrites that file
at the next start, so each run is saved right after it ends.

    py tools/perf_test.py save on      after a run with the ASTRO app open
    py tools/perf_test.py save off     after a run with the ASTRO app closed
    py tools/perf_test.py report       compares the saved runs, race by race

Whether ASTRO really ran is not taken from the label: the report checks ASTRO's log for activity during the run.
"""
import datetime as dt
import os
import re
import shutil
import sys
from pathlib import Path

APPDATA = Path(os.environ["APPDATA"]) / "ASTRO-CTR"
RUNS = APPDATA / "perf_runs"
ASTRO_LOG = APPDATA / "astro_ctr.log"
SETTLE_S = 15   # seconds after a level starts loading that are left out (loading, not racing)


def game_dir():
    p = APPDATA / "game_dir.txt"
    if p.exists():
        return Path(p.read_text(encoding="utf-8").strip())
    sys.exit("game folder unknown: open the ASTRO app once while the game runs")


def save(label):
    logs = sorted(game_dir().glob("last_run*.log"), key=lambda p: p.stat().st_mtime)
    if not logs:
        sys.exit("no last_run*.log in " + str(game_dir()))
    src = logs[-1]
    text = src.read_text(encoding="utf-8", errors="replace")
    m = re.match(r"--- SacredFire launch (\S+ \S+)", text)
    stamp = m.group(1).replace(" ", "_").replace(":", "-") if m else "unknown"
    if "--- the game ended with code" not in text:
        print("note: the game is still running (or the log is cut); close the game first for a complete run")
    RUNS.mkdir(parents=True, exist_ok=True)
    dst = RUNS / f"{label}_{stamp}.log"
    shutil.copyfile(src, dst)
    print("saved", dst)


def parse(path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    start = dt.datetime.strptime(re.match(r"--- SacredFire launch (\S+ \S+)", lines[0]).group(1), "%Y-%m-%d %H:%M:%S")
    perf, level, level_at, n_perf, limit = [], "start", -10**9, 0, 0
    for l in lines:
        if m := re.search(r"frame limit (\d+) FPS", l):
            limit = int(m.group(1))
        if l.startswith("LOADING LEVEL:"):
            level = l.split("'")[1].split("/")[-1].lower()
            level_at = n_perf
        elif l.startswith("Perf:"):
            f = l.split()
            n_perf += 1
            # windows since the level began loading (5 s each)
            perf.append((level, (n_perf - level_at) * 5, int(f[2]), float(f[4]), float(f[6])))
    end = start + dt.timedelta(seconds=5 * (n_perf + 2))
    return start, end, perf, limit


def astro_active(start, end):
    if not ASTRO_LOG.exists():
        return 0
    n = 0
    for l in ASTRO_LOG.open(encoding="utf-8", errors="replace"):
        try:
            t = dt.datetime.strptime(l[:19], "%Y-%m-%d %H:%M:%S")
        except ValueError:
            continue
        if start <= t <= end:
            n += 1
    return n


def report():
    runs = sorted(RUNS.glob("*.log"))
    if not runs:
        sys.exit("no saved runs: play a race, close the game, then: py tools/perf_test.py save on|off")
    print(f"{'run':38} {'ASTRO lines':>11} {'track':28} {'race s':>6} {'avg fps':>7} {'low 5s fps':>10} {'slow windows':>12} {'worst ms':>8}")
    for r in runs:
        start, end, perf, limit = parse(r)
        active = astro_active(start, end)
        tracks = {}
        for level, since, frames, ms, worst in perf:
            if not re.match(r"t\d+_", level) or since <= SETTLE_S:
                continue
            tracks.setdefault(level, []).append((frames, ms, worst))
        if not tracks:
            print(f"{r.stem:38} {active:>11} (no race in this run)")
        for level, w in tracks.items():
            frames = sum(x[0] for x in w)
            secs = sum(x[1] for x in w) / 1000
            low = min(x[0] / (x[1] / 1000) for x in w)
            slow = sum(1 for x in w if x[0] / (x[1] / 1000) < 0.98 * limit)
            print(f"{r.stem:38} {active:>11} {level + f' (limit {limit})':28} {secs:6.0f} {frames / secs:7.1f} {low:10.1f} {slow:>5} of {len(w):<4} {max(x[2] for x in w):8.1f}")
    print("\nASTRO lines = ASTRO's log lines during the run (0 = ASTRO did not run).")
    print("slow windows = 5-second windows under 98% of the run's frame limit; worst ms = the longest single frame.")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "save" and sys.argv[2] in ("on", "off"):
        save(sys.argv[2])
    elif len(sys.argv) >= 2 and sys.argv[1] == "report":
        report()
    else:
        print(__doc__)
