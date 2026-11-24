# Builds the driver (character) catalogue for the MAPS page from the game archives -> <out>/driver_catalog.json
# usage: py -I tools/driver_catalog_build.py <game archives dir> <out dir>
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
COLOUR = re.compile(r'(?i)(_c|_d|basecolor|diffuse)$')

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
paks = sorted(f for f in os.listdir(src) if f.lower().startswith('driver') and f.lower().endswith('.pak'))
for pak in paks:
    try: ar = Archive(os.path.join(src, pak))
    except Exception: continue
    for f in ar.files:
        p = f['path']
        if not p.startswith('textures/'): continue
        short = p.split('!')[-1].split('`')[0]
        if not COLOUR.search(short): continue
        m = re.match(r'(?i)driver([a-z0-9]+?)(_outfit_(\w+?))?(_materials|_skins)?\.pak$', pak)
        who = (m.group(1) if m else pak[6:-4]).lower()
        group = short
        pay, w, h = texture_info(ar.read(f) if hasattr(ar, 'read') else ar.extract(f))
        fm = fmt_of(pay, w, h)
        if not fm: skipped['format'] += 1; continue
        pr = probes(pay)
        if not pr: skipped['flat'] += 1; continue
        off, ch = probe(pay)
        fnv = '%016x' % fnv1a64(pay)
        found[short + '@' + fnv] = {'name': short, 'kind': 'colour', 'w': w, 'h': h, 'len': len(pay), 'head': pay[:16].hex(), 'probe_off': off,
                                    'probe': ch.hex(), 'sha1': hashlib.sha1(pay).hexdigest(), 'fnv': fnv, 'probes': pr, 'wheel': True,
                                    'fmt': fm, 'group': group, 'pak_who': who}
    del ar

texs = sorted(found.values(), key=lambda t: (t['pak_who'], t['name']))
by = {}
for t in texs: by.setdefault(t['pak_who'], []).append(t)
entries = [{'name': w, 'display': w, 'textures': ts, 'effects': [], 'tints': []} for w, ts in sorted(by.items())]
json.dump({'wheels': entries}, open(os.path.join(out, 'driver_catalog.json'), 'w'), separators=(',', ':'))
print(len(texs), 'textures', len(entries), 'characters', skipped)
