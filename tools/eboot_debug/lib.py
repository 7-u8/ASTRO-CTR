# Usage: refs.py NAME...  -> for each exact C string NAME in the USA eboot: its address, every rip-relative reference to it,
# and the instructions around each reference.
import struct, sys, re, capstone, numpy as np
P = r"C:/Users/PC/Downloads/CTRNF USA/CTRNF USA/out/eboot.elf"
d = open(P, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20); phn, = struct.unpack_from('<H', d, 0x38)
segs = []
for i in range(phn):
    t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
    if t in (1, 0x61000010) and fs: segs.append((va, off, fs, fl))
code = [s for s in segs if s[3] & 1][0]; cva, coff, cfs, _ = code; C = d[coff:coff + cfs]
def f2v(o):
    for va, off, fs, fl in segs:
        if off <= o < off + fs: return va + o - off
def rd(va, n):
    for sva, off, fs, fl in segs:
        if sva <= va < sva + fs: return d[off + va - sva: off + va - sva + n]
    return b''
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
disp = np.frombuffer(C[:len(C) - len(C) % 1], dtype=np.uint8)
# all int32 at every offset, vectorised
i32 = np.lib.stride_tricks.sliding_window_view(disp, 4)
vals = (i32[:, 0].astype(np.int64) | (i32[:, 1].astype(np.int64) << 8) | (i32[:, 2].astype(np.int64) << 16) | (i32[:, 3].astype(np.int64) << 24))
vals = np.where(vals >= 1 << 31, vals - (1 << 32), vals)
pos = np.arange(len(vals), dtype=np.int64) + cva
def refs(target):
    out = []
    for tail in (4, 5, 8):
        hits = np.nonzero(pos + tail + vals == target)[0]
        for h in hits:
            for back in range(2, 8):
                s = int(h) - back
                for ins in md.disasm(C[s:s + 16], cva + s, 1):
                    if ins.size == back + tail and 'rip' in ins.op_str: out.append(cva + s)
    return sorted(set(out))
def dis(va, before=0x30, after=0x40):
    # start from a guessed boundary: disassemble from va-before and keep the stream that hits va
    for b in range(before, before + 16):
        lst = list(md.disasm(C[va - b - cva: va + after - cva], va - b))
        if any(x.address == va for x in lst): return lst
    return list(md.disasm(C[va - cva: va + after - cva], va))
def cstr(va):
    b = rd(va, 80); z = b.find(b'\0'); s = b[:z if z >= 0 else 80]
    return s.decode('latin1') if s and all(32 <= c < 127 for c in s) else None
def ann(ins):
    t = ''
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            a = ins.address + ins.size + op.mem.disp; s = cstr(a); t = f'  ; {a:#x}' + (f' "{s}"' if s else '')
    return t

def ptr_hits(va):
    b = struct.pack('<Q', va); return [f2v(m.start()) for m in re.finditer(re.escape(b), d) if f2v(m.start()) is not None]
def q(va): return struct.unpack('<Q', rd(va, 8))[0]
