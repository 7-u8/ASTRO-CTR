# Builds the lava/fire/volcanic-rock catalogue for the MAPS page from the game archives -> <out>/lava_catalog.json
# usage: py -I tools/lava_catalog_build.py <game archives dir> <out dir>
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
# lava / magma, fire / flames / embers, volcanic rock (basalt, obsidian, scorched, ash ...): the colour maps and their glow (_E/_EC/_EP)
LAVA = re.compile(r'(?i)(lava|magma|molten)')
FIRE = re.compile(r'(?i)(^|[_!`\W])(fire|flame|flames|ember|embers|burn|burning)(\d*|_|$)')
ROCK = re.compile(r'(?i)(volcan|basalt|obsidian|scorch|charred|cinder|lavarock|magmarock|hotrock|hot_rock|(^|[_!`\W])ash(\d*|_|$)|inferno_(island_)?(stone|rock))')
COLOUR = re.compile(r'(?i)(_c|_d|_e|_ec|_ep|diffuse)$')

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

# The big lava surfaces use textures that carry no lava word in their name (Barin Ruins: the ground lava material
# T222_BG_ground_Lava uses C1_Path_Glow_C from Crash 1): so every material instance whose NAME has lava/magma/molten
# also contributes the colour textures it uses (its ColorMap entries and the emissive variant of its Composite entries).
MATNAME = re.compile(r'(?i)(lava|magma|molten)')
mat_tex = set()
for pak in sorted(f for f in os.listdir(src) if f.lower().endswith('.pak')):
    try: ar0 = Archive(os.path.join(src, pak))
    except Exception: continue
    for f in ar0.files:
        if not f['path'].startswith('materialinstances') or not MATNAME.search(f['path'].split('/')[-1]): continue
        try: strs = igz3.Igz(ar0.read(f) if hasattr(ar0, 'read') else ar0.extract(f)).strings
        except Exception: continue
        for st in strs:
            m = re.match(r'(ColorMap|Composite3HalfRes)[A-Za-z0-9]*,textures@!(.*)`', st)
            if not m: continue
            name = m.group(2).split('!')[-1]
            if '{' in name:   # e.g. T222_LavaFlow01_{E,G}: the emissive one
                name = re.sub(r'\{([A-Za-z]+),[A-Za-z]+\}', r'', name)
                if not name.endswith('_E'): continue
            elif m.group(1) != 'ColorMap': continue
            mat_tex.add(name)
    del ar0
print('textures used by lava materials:', len(mat_tex), sorted(mat_tex)[:30])
found = {}
skipped = {'flat': 0, 'format': 0}
paks = sorted(f for f in os.listdir(src) if f.lower().endswith('.pak'))
for pak in paks:
    try: ar = Archive(os.path.join(src, pak))
    except Exception: continue
    for f in ar.files:
        p = f['path']
        if not p.startswith('textures/'): continue
        short = p.split('!')[-1].split('`')[0]
        if not COLOUR.search(short): continue
        group = 'lava' if (LAVA.search(short) or (short in mat_tex and re.search(r'(?i)(glow|lava|magma|molten|fire|hot_)', short))) else 'rock' if ROCK.search(short) else 'fire' if FIRE.search(short) else None
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
json.dump({'wheels': [{'name': 'Lava', 'display': 'Lava, fire and volcanic rock', 'textures': texs, 'effects': [], 'tints': []}]}, open(os.path.join(out, 'lava_catalog.json'), 'w'), separators=(',', ':'))
from collections import Counter
print(len(texs), 'textures', dict(Counter(t['group'] for t in texs)), dict(Counter(t['fmt'] for t in texs)))
for g in ('lava', 'fire', 'rock'): print(g, sorted({t['name'] for t in texs if t['group'] == g})[:40])
print('skipped: flat', skipped['flat'], ', other formats', skipped['format'])
