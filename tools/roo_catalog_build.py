# Builds the Roo's Tubes catalogue for the MAPS page from the game archives -> <out>/roo_catalog.json
# usage: py -I tools/roo_catalog_build.py <game archives dir> <out dir>
# Textures whose name has a snow or ice word and is a colour map (_C / _D / diffuse): BC1, BC3 (16-byte blocks, 8 alpha
# bytes + a BC1 colour block) or RGBA8. Same entry format as the wheel and sky catalogues (the layer's texture engine
# finds them in memory by their bytes), plus "group": "snow" or "ice".
# Checked 2026-11-01: most are BC3 (decoding the second 8 bytes of each 16-byte block gives the snow picture).
import sys, os, re, json, struct, hashlib
import numpy as np
sys.path.insert(0, r"C:/Users/PC/Documents/all know about ctrnf/examples/core")
from archive_reader import Archive
import igz3

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
GROUPS = [('ground', re.compile(r'(?i)^(T142_(Ground_Sand|Mud|Rocks_Mud)\w*_[cd]|C3_Sea_SandUnderwater\w*_d)$')),
          ('water', re.compile(r'(?i)^(T112_Water_Path|FXT_WaterCaustic\w*|fxt_anim_caustic|Sky1_Water)$')),
          ('light', re.compile(r'(?i)^(C3_metal_trim_(rust_)?red\w*_c|C3_Sea_RedCoral\w*_c|T112_Glow_Worm\w*_c|bigglow\w*)$'))]

def texture_info(raw):
    g = igz3.Igz(raw)
    o, l = g.secs[2]
    w = h = 0
    for ob in g.objs:
        if g.tname(ob) == 'igImage2':
            w, h = struct.unpack_from('<HH', g.data, ob + 0x18)
    return raw[o:o + l], w, h

def fmt_of(pay, w, h):
    if not (w and h): return None
    r = len(pay) / (w * h)
    if 0.5 <= r <= 0.85: return 'bc1'
    if 1.0 <= r <= 1.4: return 'bc3'     # BC3 (checked by decoding); a BC7 texture would look like this too
    if 4.0 <= r <= 5.4: return 'rgba8'
    return None

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

found = {}
skipped = {'flat': 0, 'format': 0}
paks = ['t112_roos_tubes.pak']
for pak in paks:
    try: ar = Archive(os.path.join(src, pak))
    except Exception: continue
    for f in ar.files:
        p = f['path']
        if not p.startswith('textures/'): continue
        short = p.split('!')[-1].split('`')[0]
        group = next((g for g, rx in GROUPS if rx.search(short)), None)
        if not group: continue
        pay, w, h = texture_info(ar.read(f) if hasattr(ar, 'read') else ar.extract(f))
        fm = fmt_of(pay, w, h)
        if not fm: skipped['format'] += 1; continue
        pr = probes(pay)
        if not pr: skipped['flat'] += 1; continue
        off, ch = probe(pay)
        fnv = '%016x' % fnv1a64(pay)
        found[short + '@' + fnv] = {'name': short, 'kind': 'colour', 'w': w, 'h': h, 'len': len(pay), 'head': pay[:16].hex(), 'probe_off': off,
                                    'probe': ch.hex(), 'sha1': hashlib.sha1(pay).hexdigest(), 'fnv': fnv, 'probes': pr, 'wheel': True,
                                    'fmt': fm, 'group': group}
    del ar

texs = sorted(found.values(), key=lambda t: (t['group'], t['name']))
entries = [{'name': g.capitalize(), 'display': g, 'textures': [t for t in texs if t['group'] == g], 'effects': [], 'tints': []} for g, _ in GROUPS]
json.dump({'wheels': [e for e in entries if e['textures']]}, open(os.path.join(out, 'roo_catalog.json'), 'w'), separators=(',', ':'))
for e in entries:
    from collections import Counter
    print(e['name'], len(e['textures']), dict(Counter(t['fmt'] for t in e['textures'])), [t['name'] for t in e['textures']])
print('skipped', skipped)
