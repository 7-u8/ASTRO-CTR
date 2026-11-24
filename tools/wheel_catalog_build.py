# Builds the wheel catalogue for the Wheel Editor from the game archives:
#   <out>/wheel_catalog.json : every wheel pack: its colour (C) and glow (E) textures (BC1 only) with the bytes the
#                              layer uses to find them in game memory, and the effect files the pack carries
#   <out>/wheels/<name>.png  : a preview of each wheel's colour texture
# usage: py -I tools/wheel_catalog_build.py <game archives dir> <out dir>
# A texture's payload (igz section 2) is copied unchanged into game memory (seen in game 2026-10-31:
# KartLimited4_Wheels_C at 0x1089160000 matched the file byte for byte), so it can be found by its bytes.
import sys, os, json, struct, tempfile, hashlib
import numpy as np
from PIL import Image
sys.path.insert(0, r"C:/Users/PC/Documents/all know about ctrnf/examples/core")
from archive_reader import Archive
import igz3

src, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(out, 'wheels'), exist_ok=True)
tmpdir = tempfile.mkdtemp(prefix='astro_wheels_')

def rd(ar, f): return ar.read(f) if hasattr(ar, 'read') else ar.extract(f)

def morton(x, y):
    r = 0
    for i in range(3): r |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return r

def decode_bc1_mip0(pay, w, h):
    """mip 0 of a PS4 BC1 texture: 8x8-block micro tiles in Morton order, tiles row-major"""
    bw, bh = w // 4, h // 4
    tw = max(1, (bw + 7) // 8)
    blocks = np.frombuffer(pay[:tw * max(1, (bh + 7) // 8) * 64 * 8], dtype='<u4').reshape(-1, 2)
    img = np.zeros((h, w, 3), np.uint8)
    def c565(c): return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.int32)
    for by in range(bh):
        for bx in range(bw):
            i = (by // 8 * tw + bx // 8) * 64 + morton(bx % 8, by % 8)
            w0, bits = int(blocks[i, 0]), int(blocks[i, 1])
            c0, c1 = w0 & 0xffff, w0 >> 16
            p0, p1 = c565(np.array(c0)), c565(np.array(c1))
            pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3] if c0 > c1 else [p0, p1, (p0 + p1) // 2, np.zeros(3, np.int32)]
            for k in range(16):
                img[by * 4 + k // 4, bx * 4 + k % 4] = pal[(bits >> (2 * k)) & 3]
    return Image.fromarray(img)

def texture_info(raw):
    g = igz3.Igz(raw)
    o, l = g.secs[2]
    pay = raw[o:o + l]
    w = h = 0
    for ob in g.objs:
        if g.tname(ob) == 'igImage2':
            w, h = struct.unpack_from('<HH', g.data, ob + 0x18)
    return pay, w, h

def is_bc1(pay, w, h):
    base = w * h // 2
    return w and h and base <= len(pay) <= base * 1.6

def fnv1a64(b):
    # whole-payload fingerprint the layer checks before it treats memory as this texture
    h = 0xcbf29ce484222325
    for x in np.frombuffer(b[:len(b) // 8 * 8], dtype='<u8'):
        h = ((h ^ int(x)) * 0x100000001b3) & 0xffffffffffffffff
    return h

def effect_tints(raw):
    """Colours in a wheel's customisation data that its effects use as tint (drift sparks, boost flare...): a 16-byte
    aligned vec4 with alpha 1, a clear hue and non-round values. The game keeps a copy of the object in memory, so the
    same 16 bytes are found there (seen in game 2026-10-31: (0.204, 0.529, 1, 1) of KartTeamCrash_Premium)."""
    g = igz3.Igz(raw)
    d = g.data
    out, seen = [], set()
    for i in range(0, len(d) - 16, 16):
        v = struct.unpack_from('<4f', d, i)
        if v[3] == 1.0 and all(0 <= x <= 1 for x in v[:3]) and max(v[:3]) - min(v[:3]) > 0.3 and any(round(x * 8) / 8 != x for x in v[:3]):
            h = d[i:i + 16].hex()
            if h not in seen:
                seen.add(h)
                out.append({'o': i, 'c': h})
    return out

def probes(pay, n=24):
    # up to n 64-byte windows spread over the payload, each with varied blocks (never a flat colour: flat windows match
    # unrelated zeroed memory). A texture in game memory can differ from the file in a few places, so it is recognised
    # when 90% of its windows match. A texture without at least 8 varied windows has none and is not edited.
    cand = [off for off in range(0, len(pay) - 64, 64) if len({pay[off:off + 64][i:i + 8] for i in range(0, 64, 8)}) >= 7]
    if len(cand) < 8: return []
    step = max(1, len(cand) // n)
    return [{'o': o, 'h': pay[o:o + 64].hex()} for o in cand[::step][:n]]

def probe(pay):
    # a 64-byte window with varied blocks (not a flat colour), used to confirm a match
    for off in range(0, len(pay) - 64, 256):
        ch = pay[off:off + 64]
        if len({ch[i:i + 8] for i in range(0, 64, 8)}) >= 6: return off, ch
    return 0, pay[:64]

# names as the game shows them (the game's text lists each set in order; ids 1..8 follow the order of the names)
DISPLAY = {'KartLimited4': 'KartLimited4', 'KartTeamCrash_Premium': 'Electron Team Bandicoot'}
for pack, names in [('Magical', ['Blue', 'Aqua', 'Green', 'Orange', 'Pink', 'Purple', 'Red', 'Yellow']),
                    ('FireRock', ['Blue', 'Aqua', 'Green', 'Orange', 'Pink', 'Purple', 'Red', 'Yellow'])]:
    for key, nm in zip(['Blue', 'Cyan', 'Green', 'Orange', 'Pink', 'Purple', 'Red', 'Yellow'], names):
        DISPLAY['%s_%s' % (pack, key)] = ('Twilight ' if pack == 'Magical' else '') + nm + ('' if pack == 'Magical' else ' Lava Rock')
for i, nm in enumerate(['Fire', 'Ice', 'Magic', 'Lightning', 'Light', 'Darkness', 'Wind', 'Nature'], 1):
    DISPLAY['GP3_Gem_%d' % i] = nm + ' Elemental'
for i, nm in enumerate(['Blue', 'Aqua', 'Green', 'Yellow', 'Orange', 'Red', 'Pink', 'Purple'], 1):
    DISPLAY['GP8_BAP_%d' % i] = 'Atomic ' + nm
for i in range(1, 11):
    DISPLAY['GP4_Ghost_%d' % i] = 'Spectral %d' % i
wheels = []
for pak in ['update.pak', 'update2.pak']:
    top = Archive(os.path.join(src, pak))
    for f in top.files:
        p = f['path']
        if not (os.path.basename(p).startswith('KartCustomization_Wheel') and p.endswith('.pak')): continue
        name = os.path.basename(p)[:-4].replace('KartCustomization_Wheels_', '').replace('KartCustomization_Wheel_', '')
        tmp = os.path.join(tmpdir, os.path.basename(p))
        open(tmp, 'wb').write(rd(top, f))
        ar = Archive(tmp)
        # textures the wheel materials use (the wheel's own material instances)
        used = set()
        for g in ar.files:
            gp = g['path']
            if gp.startswith('materialinstances/') and 'Wheel' in gp:
                try: used.update(s for s in igz3.Igz(rd(ar, g)).strings if s.startswith(('ColorMap', 'ColorMapHalfRes')))
                except Exception: pass
        texs, preview, preview_wheelish = [], None, False
        for g in ar.files:
            gp = g['path']
            if not gp.startswith('textures/'): continue
            tname = gp[len('textures/'):-4]
            if tname not in used or 'default_' in tname: continue
            short = tname.split('!')[-1].split('`')[0]
            kind = 'glow' if short.lower().endswith('_e') else 'colour'
            pay, w, h = texture_info(rd(ar, g))
            if not is_bc1(pay, w, h): continue
            off, ch = probe(pay)
            texs.append({'name': short, 'kind': kind, 'w': w, 'h': h, 'len': len(pay),
                         'head': pay[:16].hex(), 'probe_off': off, 'probe': ch.hex(),
                         'sha1': hashlib.sha1(pay).hexdigest(), 'fnv': '%016x' % fnv1a64(pay), 'probes': probes(pay)})
            wheelish = any(k in short.lower() for k in ('wheel', 'tire', 'tyre', 'rim'))
            texs[-1]['wheel'] = wheelish   # False: a kart texture the wheel model also uses (editing it recolours the kart too)
            if kind == 'colour' and (preview is None or (wheelish and not preview_wheelish)):
                try: preview, preview_wheelish = decode_bc1_mip0(pay, w, h).resize((160, 160), Image.LANCZOS), wheelish
                except Exception as e: print('preview failed', name, e)
        vfx = sorted({os.path.basename(g['path'])[:-4] for g in ar.files if g['path'].startswith('vfx/')})
        tints = []
        for g in ar.files:
            if g['path'].startswith('KartCustomization/') and g['path'].endswith('.igz') and '_pkg' not in g['path']:
                try: tints = effect_tints(rd(ar, g))
                except Exception as e: print('tints failed', name, e)
        del ar   # the archive keeps its file open; the temp folder is left for the system to clean
        if not texs: continue
        if preview: preview.save(os.path.join(out, 'wheels', name + '.png'))
        if not any(t['wheel'] and t['kind'] == 'colour' for t in texs):
            # the pack has no texture named like a wheel: its own colour textures are the wheel's (smoke and fx sheets excluded)
            for t in texs:
                if t['kind'] == 'colour' and not any(k in t['name'].lower() for k in ('fxt_', 'smoke', 'fx_')): t['wheel'] = True
        texs.sort(key=lambda t: (not t['wheel'], t['kind'] != 'colour'))
        wheels.append({'name': name, 'display': DISPLAY.get(name, ''), 'textures': texs, 'effects': vfx, 'tints': tints})
        print(name, [t['name'] for t in texs], len(vfx), 'effects')

json.dump({'wheels': sorted(wheels, key=lambda w: w['name'])}, open(os.path.join(out, 'wheel_catalog.json'), 'w'), separators=(',', ':'))
print(len(wheels), 'wheels')
