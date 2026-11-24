# Builds the sky catalogue for the MAPS page from the game archives:
#   <out>/sky_catalog.json : one entry per map with its skybox textures (cube faces, clouds, stars...) in the wheel
#                            catalogue format, so the layer's texture engine (wheel_editor.cpp) finds and recolours
#                            them in game memory the same way it does wheels.
# usage: py -I tools/sky_catalog_build.py <game archives dir> <out dir>
# The skies are BC1 textures under levels/Octane/Global/Skybox/<map>/ (T111_Crash_Cove: back, front, left, right
# 1024x512 and up 1024x1024, single level, checked 2026-11-01).
import sys, os, json, struct, hashlib, tempfile
import numpy as np
sys.path.insert(0, r"C:/Users/PC/Documents/all know about ctrnf/examples/core")
from archive_reader import Archive
import igz3

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

def rd(ar, f): return ar.read(f) if hasattr(ar, 'read') else ar.extract(f)

# the helpers below are the same as tools/wheel_catalog_build.py (the layer checks the same fields)
def texture_info(raw):
    g = igz3.Igz(raw)
    o, l = g.secs[2]
    w = h = 0
    for ob in g.objs:
        if g.tname(ob) == 'igImage2':
            w, h = struct.unpack_from('<HH', g.data, ob + 0x18)
    return raw[o:o + l], w, h

def is_bc1(pay, w, h):
    base = w * h // 2
    return w and h and base <= len(pay) <= base * 1.6

def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in np.frombuffer(b[:len(b) // 8 * 8], dtype='<u8'):
        h = ((h ^ int(x)) * 0x100000001b3) & 0xffffffffffffffff
    return h

def probes(pay, n=24):
    cand = [off for off in range(0, len(pay) - 64, 64) if len({pay[off:off + 64][i:i + 8] for i in range(0, 64, 8)}) >= 7]
    if len(cand) < 8: return []
    step = max(1, len(cand) // n)
    return [{'o': o, 'h': pay[o:o + 64].hex()} for o in cand[::step][:n]]

def probe(pay):
    for off in range(0, len(pay) - 64, 256):
        ch = pay[off:off + 64]
        if len({ch[i:i + 8] for i in range(0, 64, 8)}) >= 6: return off, ch
    return 0, pay[:64]

maps = {}   # map folder -> {texture id: entry}
# every archive: the base maps keep their skies in their own level archive (t112_roos_tubes.pak ...), later ones in
# the updates. Updates come last so their version of a texture is added too (both are kept; the layer picks the copy
# that matches memory).
paks = sorted(f for f in os.listdir(src) if f.lower().endswith('.pak'))
paks = [f for f in paks if not f.lower().startswith('update')] + [f for f in paks if f.lower().startswith('update')]
for pak in paks:
    path = os.path.join(src, pak)
    try: ar = Archive(path)
    except Exception as e:
        print('skip', pak, e); continue
    for f in ar.files:
        p = f['path']
        if not p.startswith('textures/'): continue
        parts = p[len('textures/'):-4].split('!')
        short = parts[-1].split('`')[0]
        low = [x.lower() for x in parts]
        # the Skybox folder (T111_Crash_Cove/...), or a texture named *_Skybox_* elsewhere (T233, T241, T243, T152 keep
        # theirs outside that folder): grouped by its map code then
        if 'skybox' in low and low.index('skybox') + 2 < len(parts): folder = parts[low.index('skybox') + 1]
        elif '_skybox_' in short.lower(): folder = short.split('_')[0].upper()
        else: continue
        pay, w, h = texture_info(rd(ar, f))
        rgba = bool(w and h and w * h * 4 <= len(pay) <= w * h * 4 * 1.4)   # uncompressed RGBA8 (T152, T233, T241, T243)
        if not rgba and not is_bc1(pay, w, h): continue
        pr = probes(pay)
        if not pr: continue   # a flat texture cannot be told apart from other memory: never edited
        off, ch = probe(pay)
        fnv = '%016x' % fnv1a64(pay)
        maps.setdefault(folder, {})[short + '@' + fnv] = {
            'name': short, 'kind': 'colour', 'w': w, 'h': h, 'len': len(pay), 'head': pay[:16].hex(), 'probe_off': off,
            'probe': ch.hex(), 'sha1': hashlib.sha1(pay).hexdigest(), 'fnv': fnv, 'probes': pr, 'wheel': True, 'fmt': 'rgba8' if rgba else 'bc1'}
    del ar

def display(folder):
    # "T111_Crash_Cove" -> "Crash Cove"
    words = folder.split('_')
    return ' '.join(words[1:]) if len(words) > 1 and words[0][:1] in 'TAta' and words[0][1:].isdigit() else folder.replace('_', ' ')

skies = [{'name': k, 'display': display(k), 'textures': sorted(v.values(), key=lambda t: t['name']), 'effects': [], 'tints': []}
         for k, v in sorted(maps.items())]
json.dump({'wheels': skies}, open(os.path.join(out, 'sky_catalog.json'), 'w'), separators=(',', ':'))
for s in skies: print(s['name'], [t['name'] for t in s['textures']])
print(len(skies), 'maps,', sum(len(s['textures']) for s in skies), 'textures')
