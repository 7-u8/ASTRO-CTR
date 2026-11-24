# Every debug-menu item: a call into the menu API (0x388000..0x392000) with a label string in rsi.
# Linear sweep of the code; tracks the last rip-lea into each register.
import collections, json
exec(open(__file__.replace('menu.py', 'lib.py')).read())
md.detail = True
LO, HI = 0x380000, 0x3d0000
items = []; calls = collections.Counter()
regs = {}
off = 0
R = capstone.x86
while off < len(C):
    got = False
    for ins in md.disasm(C[off:off + 0x10000], cva + off):
        got = True; off = ins.address + ins.size - cva
        ops = ins.operands
        if ins.mnemonic == 'lea' and len(ops) == 2 and ops[1].type == R.X86_OP_MEM and ops[1].mem.base == R.X86_REG_RIP:
            regs[ops[0].reg] = (ins.address + ins.size + ops[1].mem.disp, ins.address)
        elif ins.mnemonic == 'call' and ops and ops[0].type == R.X86_OP_IMM:
            t = ops[0].imm
            if LO <= t < HI:
                lab = regs.get(R.X86_REG_RSI); arg = regs.get(R.X86_REG_RDX)
                if lab and ins.address - lab[1] < 0x30 and cstr(lab[0]):
                    calls[t] += 1
                    items.append((ins.address, t, cstr(lab[0]), (arg[0] if arg and ins.address - arg[1] < 0x30 else None)))
            regs = {}
        elif ins.mnemonic in ('ret', 'jmp'):
            regs = {}
    if not got: off += 1
print('API functions by use:', [(hex(k), v) for k, v in calls.most_common(25)])
json.dump(items, open('items.json', 'w'))
print(len(items), 'items')
