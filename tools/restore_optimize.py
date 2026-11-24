"""Puts back what ASTRO's optimize options changed (they gave no speed-up, measured 2026-11-01).

    py -I tools/restore_optimize.py

1. %APPDATA%\\ASTRO-CTR\\optimize.json -> neutral (no hidden passes or shaders, full texture detail, nothing forced).
   The old file is kept as optimize.json.before_revert_<time>.
2. The launcher's bbport.ini: the six effect_* switches ASTRO's Optimize card turned off (dynamic shadows, SSAO, depth of
   field, motion blur, chromatic aberration, anti-aliasing) -> back to 1, the value ASTRO's own backup
   (bbport.ini.astro_backup, taken before ASTRO first touched the file) holds. render_res and fps_limit are NOT changed:
   they may be your own choices. The old file is kept as bbport.ini.before_revert_<time>.
Close the game first; the app (ASTRO_CTR.exe) should be closed and opened again afterwards so it shows the new state.
"""
import json, os, re, shutil, sys, time

GAME = r"C:\Users\PC\Downloads\CTRNF EU (1)\CTRNF EU"
APP = os.path.join(os.environ['APPDATA'], 'ASTRO-CTR')
KEYS = ['effect_dynamic_shadows', 'effect_ssao', 'effect_dof', 'effect_motion_blur', 'effect_chromatic_aberration', 'effect_game_aa']
stamp = time.strftime('%Y%m%d_%H%M%S')

def game_running():
    import subprocess
    return 'sacredfire' in subprocess.run(['tasklist', '/FI', 'IMAGENAME eq sacredfire.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout

if game_running(): sys.exit('The game is running: close it first.')

# 1. optimize.json
p = os.path.join(APP, 'optimize.json')
if os.path.exists(p): shutil.copy2(p, p + '.before_revert_' + stamp)
neutral = {'aniso_off': False, 'effects_keep': 100, 'hide_shaders': [], 'keep_instances': {}, 'reduce_shaders': [], 'skip': [], 'texture_level': 0}
open(p, 'w', encoding='utf-8').write(json.dumps(neutral, indent=1) + '\n')
print('optimize.json: neutral (old one kept)')

# 2. bbport.ini
ini = os.path.join(GAME, 'bbport.ini')
text = open(ini, 'rb').read().decode('utf-8')
shutil.copy2(ini, ini + '.before_revert_' + stamp)
changed = []
for k in KEYS:
    m = re.search(r'(?m)^' + k + r'=(.*?)(\r?)$', text)
    if not m: print('  %s: not in the file' % k); continue
    if m.group(1) != '1':
        text = text[:m.start()] + k + '=1' + m.group(2) + text[m.end():]
        changed.append((k, m.group(1)))
open(ini, 'wb').write(text.encode('utf-8'))
for k, old in changed: print('  %-30s %s -> 1' % (k, old))
print('bbport.ini: %d switches restored, nothing else touched (old file kept)' % len(changed))
for k in KEYS + ['render_res', 'fps_limit']:
    print('  now %-30s %s' % (k, re.search(r'(?m)^' + k + r'=(.*?)\r?$', text).group(1)))
