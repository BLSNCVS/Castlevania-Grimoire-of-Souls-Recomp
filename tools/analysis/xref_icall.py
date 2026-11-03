# Finds functions in the il2cpp section that reference given C strings via
# adrp+add pairs. Decodes only those two instruction forms to stay fast.
# usage: python3 -I xref_icall.py <UnityFramework> <function_table.json> <string>...
import sys, json, bisect, array
import lief
b = lief.parse(sys.argv[1]); names = sys.argv[3:]
fns = json.load(open(sys.argv[2]))["functions"]; starts = [f["addr"] for f in fns]
cs = b.get_section("__cstring"); cdata = bytes(cs.content)
targets = {}
for n in names:
    key = b"\0" + n.encode() + b"\0"
    i = cdata.find(key)
    if i >= 0: targets[cs.virtual_address + i + 1] = n
print({hex(k): v for k, v in targets.items()})
g = b.get_section("il2cpp"); words = array.array("I", bytes(g.content)); base = g.virtual_address
pages = [None] * 32
for idx, ins in enumerate(words):
    if ins & 0x9F000000 == 0x90000000:  # adrp
        imm = (((ins >> 5) & 0x7FFFF) << 2) | ((ins >> 29) & 3)
        if imm & (1 << 20): imm -= 1 << 21
        pages[ins & 31] = ((base + idx * 4) & ~0xFFF) + (imm << 12)
    elif ins & 0xFF800000 == 0x91000000:  # add x, x, #imm
        p = pages[(ins >> 5) & 31]
        if p is None: continue
        v = p + (((ins >> 10) & 0xFFF) << (12 if ins & (1 << 22) else 0))
        if v in targets:
            pc = base + idx * 4
            f = fns[bisect.bisect_right(starts, pc) - 1]
            print(targets[v], "<-", hex(pc), f["name"], hex(f["addr"]))
