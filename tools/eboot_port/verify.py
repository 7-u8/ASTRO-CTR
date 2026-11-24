import re
src = open(r'C:/Users/PC/Documents/ASTRO-CTR/mods_dll/101_story.cpp').read()
el = {'EU': open(r'C:/Users/PC/Downloads/CTRNF EU (1)/CTRNF EU/out/eboot.elf','rb').read(),
      'USA': open(r'C:/Users/PC/Downloads/CTRNF USA/CTRNF USA/out/eboot.elf','rb').read()}
def at(d, va): return 0x4000 + va   # code segment: file offset 0x4000, vaddr 0
for m in re.finditer(r'\{"(EU|USA)[^"]*", (0x[0-9a-f]+), (0x[0-9a-f]+),\s*\{([^}]*)\}, \{[^}]*\},\s*\{([^}]*)\}', src):
    tag, s1, s2, sig1, sig2 = m.groups(); d = el[tag]
    b1 = bytes(int(x, 16) for x in sig1.split(',')); b2 = bytes(int(x, 16) for x in sig2.split(','))
    o1 = at(d, int(s1, 16) - 0x400000); o2 = at(d, int(s2, 16) - 0x400000)
    assert d[o1:o1+len(b1)] == b1, (tag, 'sig1'); assert d[o2:o2+len(b2)] == b2, (tag, 'sig2')
    other = el['USA' if tag == 'EU' else 'EU']; assert b1 not in other and b2 not in other, (tag, 'sig also in other build')
    print(tag, 'ok: both signatures at their addresses, unique to this build')
