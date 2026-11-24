# Pair script-function names with their native functions: the registration fills a stack array of function pointers
# ([rbp-N], ascending) in the same order as the names table.
import sys
exec(open(__file__.replace('vsc.py', 'lib.py')).read())
def vsc(names_tab, reg_at, lo=0x500, hi=0x600):
    names = []; a = names_tab
    while cstr(q(a)): names.append(cstr(q(a))); a += 8
    regs = {}; stores = {}
    for ins in md.disasm(C[reg_at - lo - cva: reg_at + hi - cva], reg_at - lo):
        ops = ins.operands
        if ins.mnemonic == 'lea' and ops[1].type == capstone.x86.X86_OP_MEM and ops[1].mem.base == capstone.x86.X86_REG_RIP:
            regs[ops[0].reg] = ins.address + ins.size + ops[1].mem.disp
        elif ins.mnemonic == 'mov' and ops[0].type == capstone.x86.X86_OP_MEM and ops[0].mem.base == capstone.x86.X86_REG_RBP and ops[1].type == capstone.x86.X86_OP_REG and ops[1].reg in regs:
            t = regs[ops[1].reg]
            if cva <= t < cva + len(C) and not cstr(t): stores[ops[0].mem.disp] = t
    fns = [stores[k] for k in sorted(stores)]
    return names, fns
if __name__ == '__main__':
    names, fns = vsc(int(sys.argv[1], 16), int(sys.argv[2], 16))
    print(len(names), 'names', len(fns), 'fn ptrs')
    for n, f in zip(names, fns): print(f"{n:<40} {f:#x}")
