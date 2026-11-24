# Builds the vertex-colour catalogue for the ITEMS page -> <out>/mesh_catalog.json
# usage: py -I tools/mesh_catalog_build.py <game archives dir> <out dir>
# The TNT and Nitro crates are grey wood (C1_WoodCrate_gray_c, shared by every crate) coloured by their vertex colours:
# TNT ff2020ff (red) and fff220ff (yellow bands), Nitro 54c456ff (green) and fffd43ff (yellow). Their vertex buffers sit
# in a read-only page region of the game (found once each, 2026-11-01). Each entry: a 64-byte probe to find the buffer,
# and the RGBA colours (bytes r g b a) that can be replaced.
import sys, os, re, json
sys.path.insert(0, r"C:/Users/PC/Documents/all know about ctrnf/examples/core")
from archive_reader import Archive
import igz3

src, out = sys.argv[1], sys.argv[2]
MODELS = [('tnt', r'(?i)^actors/Octane/Levels/Crates/TNTCrate\.igz$', [('tnt_body', 'ff2020ff'), ('tnt_band', 'fff220ff')]),
          ('nitro', r'(?i)^models/Levels/Crash1/crates/Crash_Crate_Nitro\.igz$', [('nitro_body', '54c456ff'), ('nitro_band', 'fffd43ff')])]
ar = Archive(os.path.join(src, 'permanent.pak'))
entries = []
for name, rx, cols in MODELS:
    f = next(f for f in ar.files if re.match(rx, f['path']))
    raw = ar.read(f) if hasattr(ar, 'read') else ar.extract(f)
    g = igz3.Igz(raw)
    o, l = max(g.secs, key=lambda s: s[1])
    sec = raw[o:o + l]
    first = bytes.fromhex(cols[0][1])
    i = sec.find(first, 4096)
    probe = sec[i - 28:i + 36]
    assert len(probe) == 64
    entries.append({'name': name, 'probe': probe.hex(), 'probe_at': i - 28,
                    'colours': [{'key': k, 'rgba': c, 'count': sec.count(bytes.fromhex(c))} for k, c in cols]})
# material colours (4 floats r g b 1 in the material's command stream, one read-write copy in game memory each, seen
# 2026-11-01: writing it turned the potion liquid purple at once)
vecs = []
for key, rx, test in [('potion_green', r'fx_mat_pwrup_beaker_materials,fxm_pwrup_beaker,', lambda v: abs(v[1] - 0.929) < 0.001),
                      ('potion_red', r'fx_mat_pwrup_beaker_materials,fxm_pwrup_beaker_red,', lambda v: abs(v[0] - 0.91) < 0.001)]:
    import struct
    f = next(f for f in ar.files if rx in f['path'])
    raw = ar.read(f) if hasattr(ar, 'read') else ar.extract(f)
    for o in range(0, len(raw) - 16, 4):
        v = struct.unpack_from('<4f', raw, o)
        if v[3] == 1.0 and test(v): vecs.append({'key': key, 'value': raw[o:o + 16].hex()}); break
# shield domes: the colours of their materials (testing 2026-11-01: writing these turned the domes purple)
def shield_values(name, keep):
    out = []
    f = next(f for f in ar.files if f['path'].endswith(name))
    raw = ar.read(f) if hasattr(ar, 'read') else ar.extract(f)
    for o in range(4, len(raw) - 12, 4):
        v = struct.unpack_from('<3f', raw, o)
        if struct.unpack_from('<f', raw, o - 4)[0] == 0.0 and keep(v) and raw[o:o + 12].hex() not in [x['value'] for x in out]:
            out.append({'key': None, 'value': raw[o:o + 12].hex()})
    return out
for x in shield_values('fx_mat_pwrup_shield_materials,GreenShield,040.igz', lambda v: 0.5 < v[1] <= 1 and v[2] < 0.5 and 0 <= v[0] <= 1 and v[1] > v[0]):
    x['key'] = 'shield_green'; vecs.append(x)
for x in shield_values('materialinstances/VFX/CTR/fx_mat_pwrup_shield_materials.igz', lambda v: 0.8 < v[2] <= 1 and 0.02 < v[1] < 0.8 and 0 <= v[0] < 0.6):
    x['key'] = 'shield_blue'; vecs.append(x)
json.dump({'meshes': entries, 'vectors': vecs}, open(os.path.join(out, 'mesh_catalog.json'), 'w'), indent=1)
for e in entries: print(e['name'], [(c['key'], c['count']) for c in e['colours']])
print('vectors', vecs)
