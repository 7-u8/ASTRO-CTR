"""Did ASTRO write where the game crashed?

Reads a crashed launcher log (the Fault block: registers and fault address) and ASTRO's write journal of that game
(%APPDATA%\\ASTRO-CTR\\writes_<pid>.log), and lists every ASTRO write that touched memory within WINDOW bytes of
any heap address in the crash registers. The game objects involved in the crash sit at those addresses.

    py tools/crash_vs_writes.py <crash log> <writes_<pid>.log> [window bytes, default 0x400]
"""
import re
import sys

log, journal = sys.argv[1], sys.argv[2]
window = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x400

text = open(log, encoding='utf-8', errors='replace').read()
fault = text[text.rfind('Fault:'):]
regs = {k: int(v, 16) for k, v in re.findall(r'\b(r[a-z0-9]{1,2})=([0-9a-f]{16})', fault)}
m = re.search(r'Fault: \w+ access to ([0-9a-f]+)', fault)
targets = {('fault', int(m.group(1), 16))} if m else set()
targets |= {(k, v) for k, v in regs.items() if 0x1000000000 <= v < 0x8000000000}
print('heap addresses in the crash:', ', '.join(f'{k}={v:#x}' for k, v in sorted(targets)))

writes = []
for line in open(journal, encoding='utf-8', errors='replace'):
    f = line.split()
    if len(f) >= 4:
        writes.append((int(f[0]), f[1], int(f[2], 16), int(f[3]), line.strip()))
print(f'{len(writes)} ASTRO writes in the journal')

hits = 0
for ms, who, addr, n, raw in writes:
    for name, t in targets:
        if addr - window <= t < addr + n + window:
            hits += 1
            where = 'INSIDE the write' if addr <= t < addr + n else f'{t - addr:+#x} from it'
            print(f'  {name}={t:#x} is {where}: {raw[:180]}')
print('RESULT:', f'{hits} ASTRO writes near the crashed objects' if hits else 'no ASTRO write near any crashed object')
