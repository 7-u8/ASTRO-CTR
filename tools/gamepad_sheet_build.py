# Extracts the game's own button art for the MISC page (gamepad view) -> <out>/gamepad_sheet.png + gamepad_cells.json
# usage: py -I tools/gamepad_sheet_build.py <game dir> <out dir>
# The game draws every button prompt (Cross, Circle, L1, L2 ...) from the image "button" of its font fonts/GeekSpeak.igz
# (512x263, PS4 tiled). scripts/button_prompts.py of the launcher knows how to read it; this only uses its readers.
import sys, os, json, struct
game, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(game, 'scripts'))
import button_prompts as bp
from archive_reader import Archive
os.makedirs(out, exist_ok=True)
best = None
for pak in ('update3.pak', 'update2.pak', 'update.pak', 'permanent.pak'):
    path = os.path.join(game, 'CUSA14876-1.21', 'archives', pak)
    if not os.path.exists(path): continue
    ar = Archive(path)
    f = next((f for f in ar.files if f['path'] == 'fonts/GeekSpeak.igz'), None)
    if not f: continue
    blob = ar.read(f) if hasattr(ar, 'read') else ar.extract(f)
    try:
        w, h, at, ln = bp.image_location(blob)
        cells = bp.button_cells(blob, w, h)
    except Exception as e:
        print(pak, 'skip', e); continue
    print(pak, w, h, sorted(cells))
    if {'Cross', 'Circle', 'Triangle', 'Square', 'L1', 'R1', 'L2', 'R2'} <= set(cells):
        best = (blob, w, h, at, ln, cells, pak); break
blob, w, h, at, ln, cells, pak = best
lin = bytearray(bp.untile(blob[at:at + ln], w, h))
rgba = bp.swap_red_blue(bytes(lin))          # stored BGRA
bp.write_png(os.path.join(out, 'gamepad_sheet.png'), w, h, rgba)
json.dump({'source': pak, 'width': w, 'height': h, 'cells': {k: list(v) for k, v in cells.items()}}, open(os.path.join(out, 'gamepad_cells.json'), 'w'), indent=1)
print('ok', cells)
# the sticks' caps ("L" and "R" discs) are not button prompts of the font's metrics: located on the sheet by eye (same row as Triangle/Square)
cells['LStick'] = (272, 85, 360, 170)
cells['RStick'] = (362, 85, 450, 170)
open(os.path.join(out, 'gamepad_sheet.rgba'), 'wb').write(rgba)
json.dump({'source': pak, 'width': w, 'height': h, 'cells': {k: list(v) for k, v in cells.items()}}, open(os.path.join(out, 'gamepad_cells.json'), 'w'), indent=1)
print('rgba', len(rgba), 'sticks added')
