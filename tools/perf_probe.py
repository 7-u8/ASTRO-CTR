"""What limits the game's speed: the graphics card, or one CPU thread?

    py -I tools/perf_probe.py <label>

Waits for sacredfire.exe, then every second records: the graphics card's load and clock (nvidia-smi), the game
process' total CPU use, and the busiest threads (CPU % of one core each, with the thread's start address). Stops
when the game closes and writes %APPDATA%\\ASTRO-CTR\\probe_<label>_<time>.csv and prints a summary:
  GPU load low + one thread near 100%  -> the CPU (that thread) is the limit: graphics options cannot add speed
  GPU load high                        -> the graphics card is the limit: graphics options can help
Frames per second come from the launcher's own log (last_run*.log, "Perf:" lines every 5 s), summarised by
tools/perf_test.py; this probe is timed with the same clock so the two can be read side by side.
"""
import csv, ctypes, os, subprocess, sys, time
import psutil

_k = ctypes.WinDLL('kernel32', use_last_error=True)
_k.OpenThread.restype = ctypes.c_void_p
_names = {}
def tname(tid):
    """the name the emulator gave the thread (GetThreadDescription), or ''"""
    if tid in _names: return _names[tid]
    n = ''
    h = _k.OpenThread(0x0800, False, tid)    # THREAD_QUERY_LIMITED_INFORMATION
    if h:
        buf = ctypes.c_wchar_p()
        if _k.GetThreadDescription(ctypes.c_void_p(h), ctypes.byref(buf)) >= 0 and buf.value: n = buf.value
        _k.CloseHandle(ctypes.c_void_p(h))
    _names[tid] = n
    return n

label = sys.argv[1] if len(sys.argv) > 1 else 'run'
out = os.path.join(os.environ['APPDATA'], 'ASTRO-CTR', 'probe_%s_%s.csv' % (label, time.strftime('%H%M%S')))

def game():
    for p in psutil.process_iter(['name']):
        if (p.info['name'] or '').lower() == 'sacredfire.exe': return p
    return None

def gpu():
    r = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu,utilization.memory,clocks.sm,power.draw',
                        '--format=csv,noheader,nounits'], capture_output=True, text=True)
    try: return [float(x) for x in r.stdout.strip().split(',')]
    except Exception: return [0, 0, 0, 0]

print('waiting for the game...', flush=True)
p = None
while p is None:
    p = game(); time.sleep(1)
print('game found (pid %d): sampling every second until it closes' % p.pid, flush=True)
ncpu = psutil.cpu_count(logical=True)
prev = {t.id: t.user_time + t.system_time for t in p.threads()}
rows = []
agg = {}      # thread id -> its CPU % each second (the busiest six of each second)
t0 = time.time()
with open(out, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['t', 'gpu_pct', 'gpu_mem_pct', 'gpu_mhz', 'gpu_w', 'proc_cpu_pct_of_one_core', 'top1', 'top2', 'top3'])
    last = time.time()
    while p.is_running():
        time.sleep(1)
        now = time.time(); dt = now - last; last = now
        try: th = {t.id: t.user_time + t.system_time for t in p.threads()}
        except psutil.Error: break
        use = sorted(((th[i] - prev.get(i, th[i])) / dt * 100, i) for i in th)[::-1]
        prev = th
        g = gpu()
        total = sum(u for u, _ in use)
        row = [round(now - t0), *g, round(total)] + ['%d:%s:%d%%' % (i, tname(i) or '?', u) for u, i in use[:3]]
        w.writerow(row); f.flush(); rows.append((g[0], g[2], total, use[0][0] if use else 0))
        for u, i in use[:6]: agg.setdefault(i, []).append(u)
rows = rows[10:]    # the first seconds are loading
if rows:
    n = len(rows)
    avg = lambda k: sum(r[k] for r in rows) / n
    busy = sorted(r[3] for r in rows)
    print('%d seconds. graphics card load: average %.0f%% (busiest second %.0f%%), clock %.0f MHz' % (n, avg(0), max(r[0] for r in rows), avg(1)))
    print('game CPU: %.0f%% of one core in total (the PC has %d logical cores); the busiest thread: average %.0f%% (median %.0f%%, top 10%% of seconds above %.0f%%)'
          % (avg(2), ncpu, avg(3), busy[n // 2], busy[int(n * 0.9)]))
print('busiest threads over the run (average / 90th percentile CPU % of one core, seconds in the top 6):')
for i, v in sorted(agg.items(), key=lambda kv: -sum(kv[1]))[:8]:
    v = sorted(v); print('  thread %6d %-34s avg %3.0f%%  p90 %3.0f%%  (%d s)' % (i, tname(i) or '(no name)', sum(v) / len(v), v[int(len(v) * 0.9)], len(v)))
print('saved', out)
