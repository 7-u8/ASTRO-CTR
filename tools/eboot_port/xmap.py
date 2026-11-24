# For each EU eboot address: find the EU code that references it (rip-relative), find the same code in the USA eboot,
# and read the USA address that code references. Votes over every reference; prints the agreed USA address.
import struct, sys, collections, capstone
EU = r"C:/Users/PC/Downloads/CTRNF EU (1)/CTRNF EU/out/eboot.elf"
US = r"C:/Users/PC/Downloads/CTRNF USA/CTRNF USA/out/eboot.elf"
def load(p):
    d = open(p, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20); phn, = struct.unpack_from('<H', d, 0x38)
    segs = []
    for i in range(phn):
        t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
        if t in (1, 0x61000010): segs.append((va, off, fs, ms, fl))
    return d, segs
eu, eus = load(EU); us, uss = load(US)
def code(d, segs):
    va, off, fs, ms, fl = [s for s in segs if s[4] & 1][0]
    return va, d[off:off + fs]
ecv, ec = code(eu, eus); ucv, uc = code(us, uss)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def rip_refs(buf, base, target):
    """instructions in buf whose rip-relative operand resolves to target (brute force disp32 then confirm)."""
    out = []
    t = target
    for i in range(0, len(buf) - 4):
        disp, = struct.unpack_from('<i', buf, i)
        for tail in (4, 5, 8):           # disp is followed by 0, 1 or 4 immediate bytes
            if base + i + tail + disp == t:
                for back in range(2, 8):    # instruction starts a few bytes before the disp
                    s = i - back
                    if s < 0: continue
                    for ins in md.disasm(buf[s:s + 16], base + s, 1):
                        if ins.size == back + tail and ins.disp_offset == back if hasattr(ins, 'disp_offset') else False:
                            out.append(s)
                        elif ins.size == back + tail and ('rip' in ins.op_str):
                            out.append(s)
    return sorted(set(out))
def norm(buf, base, s, n=48):
    """bytes of n bytes of code from s with every rip displacement / rel branch masked."""
    mask = bytearray(buf[s:s + n]); pos = s
    for ins in md.disasm(buf[s:s + n + 16], base + s):
        if pos - s >= n: break
        o = pos - s
        if 'rip' in ins.op_str or ins.mnemonic.startswith(('call', 'j')):
            for k in range(max(1, ins.size - 4), ins.size):  # mask the tail (disp/rel)
                if o + k < n: mask[o + k] = 0
            if ins.mnemonic.startswith(('call','j')) and ins.size == 2 and o + 1 < n: mask[o+1] = 0
        pos += ins.size
    return bytes(mask)
def find_us(es):
    """the same instruction in the USA code: try shifts 0..0x100 in 8-byte steps both ways, compare masked windows."""
    key = norm(ec, ecv, es)
    for dlt in sorted(range(-0x200, 0x201), key=abs):
        us_s = es - ecv + dlt
        if 0 <= us_s < len(uc) - 64 and norm(uc, ucv, us_s) == key:
            return us_s
    return None
def resolve(buf, base, s):
    for ins in md.disasm(buf[s:s + 16], base + s, 1):
        x = ins.op_str
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp, f"{ins.mnemonic} {x}"
    return None, None
for a in sys.argv[1:]:
    ea = int(a, 16)
    refs = rip_refs(ec, ecv, ea)
    votes = collections.Counter(); seen = 0
    for r in refs[:40]:
        us_s = find_us(r)
        if us_s is None: continue
        t, txt = resolve(uc, ucv, us_s)
        if t is not None: votes[t] += 1; seen += 1
    best = votes.most_common(3)
    print(f"EU {ea:#x}: {len(refs)} refs, matched {seen} -> " + ", ".join(f"{t:#x}(x{c})" for t, c in best))
