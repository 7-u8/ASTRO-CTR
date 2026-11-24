import sys
exec(open(__file__.replace('readers.py', 'lib.py')).read())
def func_start(a):
    # walk back to a "push rbp; mov rbp, rsp" prologue
    for k in range(0, 0x4000):
        if C[a - k - cva: a - k - cva + 4] == b'\x55\x48\x89\xe5': return a - k
    return None
for s in sys.argv[1:]:
    name, v = s.split('=')
    rs = refs(int(v, 16))
    print(f"=== {name} {v}: {len(rs)} refs, in functions {sorted({hex(func_start(r) or 0) for r in rs})}")
