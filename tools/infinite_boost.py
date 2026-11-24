"""Infinite boost for your own kart (racer 0) in the running game (sacredfire.exe), offline races only.
Keeps every active boost of your kart at elapsed 0, so a boost you start (drift, pad, start) never ends.
It never adds boosts: adding one per frame overflows CKartBoostComponent's list and corrupts memory.
Addresses: same as src/layer/players_esp.cpp (EU and USA 1.21, guest base 0x800000000).
usage: py -3 infinite_boost.py      (Ctrl+C stops)"""
import ctypes, struct, subprocess, sys, time
from ctypes import wintypes as wt

G = 0x800000000
RACER_MGR, META_BOOST = G + 0x2ad4b70, G + 0x2ac4018
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                                                    ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def pid():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq sacredfire.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    if 'sacredfire' not in out:
        sys.exit('the game (sacredfire.exe) is not running')
    return int(out.split(',')[1].strip('"'))


h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008, False, pid())   # VM_READ | VM_WRITE | VM_OPERATION
if not h:
    sys.exit('cannot open the game process (error %d)' % ctypes.get_last_error())


def rd(a, n):
    b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    return b.raw if a and k32.ReadProcessMemory(h, a, b, n, ctypes.byref(got)) and got.value == n else None


def q(a):
    b = rd(a, 8); return struct.unpack('<Q', b)[0] if b else 0


def u(a):
    b = rd(a, 4); return struct.unpack('<I', b)[0] if b else 0


def heap(p):
    return 0x1000000000 <= p < 0x8000000000


def lst(l, cap=96):
    if not heap(l): return []
    n, d = u(l + 0xc), q(l + 0x20)
    if not 0 < n <= 4096 or not heap(d): return []
    b = rd(d, min(n, cap) * 8)
    return list(struct.unpack('<%dQ' % (len(b) // 8), b)) if b else []


meta = {}


def meta_of(c):
    vt = q(c)
    if vt not in meta:
        fn = q(vt); b = rd(fn, 8) if fn else None
        meta[vt] = fn + 7 + struct.unpack('<i', b[3:7])[0] if b and b[:3] == b'\x48\x8b\x05' and b[7] == 0xc3 else 0
    return meta[vt]


def my_boost():
    mgr = q(RACER_MGR)
    if not heap(mgr): return None, False
    if lst(q(mgr + 0x28)):          # _racerNetInfos: online race -> do nothing
        return None, True
    # your racer = the one with a local-player controller at +0x10 (the list order is not stable)
    mine = [r for r in lst(q(mgr + 0x10), 16) if heap(r) and heap(q(r + 0x10))]
    if not mine: return None, False
    hd = q(mine[0] + 0x20)
    if not heap(hd) or u(hd) & 0x30000000: return None, False
    ent = q(hd + 0x28)
    for c in lst(q(ent + 0x18)) if heap(ent) else []:
        if heap(c) and meta_of(c) == META_BOOST:
            return c, False
    return None, False


zero = struct.pack('<f', 0.0)
print('infinite boost on: start any boost in an offline race and it keeps going. Ctrl+C to stop.')
last = ''
try:
    while True:
        boost, online = my_boost()
        # hold only the newest active boost: holding them all let new ones pile up (17 seen live), the overflow risk
        act = [it for it in (lst(q(boost + 0x28), 64) if boost else [])
               if heap(it) and rd(it + 0x48, 1) == b'\x00' and q(it + 0x10)]
        held = 0
        if act:
            it = max(act, key=lambda a: u(a + 0x38))                      # latest start tick
            k32.WriteProcessMemory(h, it + 0x18, zero, 4, None)          # elapsed -> 0
            held = len(act)
        s = 'online race: off' if online else ('holding %d boost(s)' % held if held else 'waiting for a boost')
        if s != last:
            print(s); last = s
        time.sleep(0.01)
except KeyboardInterrupt:
    print('stopped')
