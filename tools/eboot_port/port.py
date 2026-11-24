# Port the 101% Story patch bytes from the EU eboot to the USA eboot: every rel32 / rip disp is re-pointed at the
# USA copy of its target (code target: same code found by masked match; data: same address, checked by xmap).
import re, struct
exec(open(__file__.replace(__file__.split('\')[-1].split('/')[-1], 'xmap.py')).read().split('for a in sys.argv')[0])
src = open(r'C:/Users/PC/Documents/ASTRO-CTR/mods_dll/101_story.cpp').read()
def arr(name): return bytes(int(x, 16) for x in re.search(name + r'\[\] = \{([^}]*)\}', src).group(1).replace(' ', '').split(',') if x)
R, S1, N1, S2, N2 = map(arr, ('kRoutine', 'kSig1', 'kNew1', 'kSig2', 'kNew2'))
X = 0x400000   # XML address - X = eboot vaddr
def ufind(sig, buf):
    return [m.start() for m in re.finditer(re.escape(sig), buf)]
def map_target(t):
    if ecv <= t < ecv + len(ec):
        if ec[t - ecv:t - ecv + 16] == bytes(16): return t      # padding/cave: same address
        s = find_us(t - ecv); assert s is not None, hex(t); return s + ucv
    return t   # data segments keep their addresses (checked)
def port(bytes_, eu_va, us_va):
    out = bytearray(bytes_); notes = []
    for ins in md.disasm(bytes_, eu_va):
        o = ins.address - eu_va
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp; nt = map_target(t)
                d = nt - (us_va + o + ins.size); struct.pack_into('<i', out, o + ins.disp_offset, d)
                notes.append(f"{ins.mnemonic} rip {t:#x}->{nt:#x}")
            elif op.type == capstone.x86.X86_OP_IMM and (ins.mnemonic.startswith(('call', 'j'))) and ins.imm_size == 4:
                t = op.imm; nt = map_target(t)
                struct.pack_into('<i', out, o + ins.imm_offset, nt - (us_va + o + ins.size))
                notes.append(f"{ins.mnemonic} {t:#x}->{nt:#x}")
            elif op.type == capstone.x86.X86_OP_IMM and ins.mnemonic.startswith('j') and ins.imm_size == 1:
                pass   # short jumps stay inside the patch
    return bytes(out), notes
def fmt(b): return ', '.join(f'0x{x:02x}' for x in b)
# routine (cave, same address)
r_va = 0x0273bf00 - X; Ru, n = port(R, r_va, r_va); print("ROUTINE", n)
# site 1: EU signature place, then the USA place of the same code
e1 = 0x01441dd9 - X; eh = ufind(S1, ec); print("EU sig1 at", [hex(h + ecv) for h in eh])
u1 = find_us(eh[0]) + ucv; S1u = uc[u1 - ucv:u1 - ucv + len(S1)]
N1u, n = port(N1, eh[0] + ecv + 9, u1 + 9); print("NEW1", n)
print("USA sig1 at", hex(u1), "unique:", len(ufind(S1u, uc)), "EU orig patch bytes there:", ec[eh[0]+9:eh[0]+14].hex(), "US:", uc[u1-ucv+9:u1-ucv+14].hex())
eh2 = ufind(S2, ec); print("EU sig2 at", [hex(h + ecv) for h in eh2])
u2 = find_us(eh2[0]) + ucv; S2u = uc[u2 - ucv:u2 - ucv + len(S2)]
N2u, n = port(N2, eh2[0] + ecv, u2); print("NEW2", n)
print("USA sig2 at", hex(u2), "unique:", len(ufind(S2u, uc)))
# check: the EU original bytes at each patch, ported, equal the USA original bytes (proves the mapping)
o1, _ = port(ec[eh[0]+9:eh[0]+14], eh[0]+ecv+9, u1+9); print("site1 EU-orig ported == USA-orig:", o1 == uc[u1-ucv+9:u1-ucv+14])
o2, _ = port(ec[eh2[0]:eh2[0]+24], eh2[0]+ecv, u2); print("site2 EU-orig ported == USA-orig:", o2 == uc[u2-ucv:u2-ucv+24])
print("\nUSA XML addresses: routine", hex(r_va + X), "site1", hex(u1 + X), "site2", hex(u2 + X))
print("kRoutine =", fmt(Ru)); print("kSig1 =", fmt(S1u)); print("kNew1 =", fmt(N1u)); print("kSig2 =", fmt(S2u)); print("kNew2 =", fmt(N2u))
