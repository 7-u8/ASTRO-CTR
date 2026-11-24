# Builds src/layer/vfx_catalog.json: every igVfxColorOperator in every VFX file of the game archives (including
# the archives nested inside update.pak / update2.pak, e.g. wheel and kart packs), keyed by
# "field|r|g|b|a" (values x10000, rounded) -> sorted effect names.
# usage: py -I tools/vfx_catalog_build.py <game archives dir> <out json>
import sys, os, struct, json, tempfile
sys.path.insert(0, r"C:/Users/PC/Documents/all know about ctrnf/examples/core")
from archive_reader import Archive
from igz3 import Igz

src, out_path = sys.argv[1], sys.argv[2]
cat, seen = {}, set()
tmpdir = tempfile.mkdtemp(prefix='astro_cat_')

def rd(ar, f): return ar.read(f) if hasattr(ar, 'read') else ar.extract(f)

def walk(ar, depth=0):
    for f in ar.files:
        p = f['path']
        if p.endswith('.pak') and depth < 2:
            tmp = os.path.join(tmpdir, 'n%d_%s' % (depth, os.path.basename(p)))
            try:
                open(tmp, 'wb').write(rd(ar, f))
                walk(Archive(tmp), depth + 1)
            except Exception as e:
                print('skip nested', p, e)
            finally:
                if os.path.exists(tmp): os.remove(tmp)
            continue
        if not (p.startswith('vfx/') and p.endswith('.igz')): continue
        name = os.path.basename(p)[:-4]
        if name in seen: continue
        seen.add(name)
        try: g = Igz(rd(ar, f))
        except Exception: continue
        for o in g.objs:
            if g.tname(o) != 'igVfxColorOperator': continue
            fld = struct.unpack_from('<I', g.data, o + 16)[0]
            col = struct.unpack_from('<4f', g.data, o + 32)
            key = str(fld) + '|' + '|'.join(str(int(round(x * 10000))) for x in col)
            cat.setdefault(key, set()).add(name)

for pak in sorted(os.listdir(src)):
    if pak.endswith('.pak'):
        try: walk(Archive(os.path.join(src, pak)))
        except Exception as e: print('skip', pak, e)
json.dump({k: sorted(v) for k, v in cat.items()}, open(out_path, 'w'), separators=(',', ':'))
print(len(seen), 'effect files,', len(cat), 'colours')
