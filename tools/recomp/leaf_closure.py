# Lists call targets of the recompiled code that nothing handles yet, and for
# each one whether its static call closure is leaf code (reaches nothing but
# itself and libc stubs) that can simply be translated too.
#
# usage: python3 -I leaf_closure.py <GameAssembly arm64> <handled.txt> <tables.json,...> <recomp dirs...>
import sys, re, glob, json, bisect
import lief, capstone

binary, handled_path, tables = sys.argv[1:4]
dirs = sys.argv[4:]
b = lief.parse(binary)
seg = next(s for s in b.segments if s.name == "__TEXT")
code, base = bytes(seg.content), seg.virtual_address
stubs = b.get_section("__stubs")
starts = sorted(set(b.function_starts.functions))
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

handled = {int(l, 16) for l in open(handled_path) if l.strip()}
known = set()
for t in tables.split(","):
    known |= {f["addr"] for f in json.load(open(t))["functions"]}


def is_stub(a):
    return stubs.virtual_address <= a < stubs.virtual_address + stubs.size


def callees(a):
    i = bisect.bisect_right(starts, a)
    end = starts[i] if i < len(starts) else base + len(code)
    out = set()
    for ins in md.disasm(code[a - base:end - base], a):
        if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
            t = int(ins.op_str[1:], 16)
            if not a <= t < end:
                out.add(t)
        if ins.mnemonic in ("blr", "br"):
            out.add(-1)
    return out


targets = set()
for d in dirs:
    for p in glob.glob(f"{d}/recomp_*.cpp"):
        for m in re.finditer(r"CallImport\(c, (0x[0-9a-f]+)\)", open(p).read()):
            targets.add(int(m.group(1), 16))
todo = sorted(t for t in targets if t not in handled and t not in known)
leaf_all = set()
for t in todo:
    seen, stack, leaf = set(), [t], True
    while stack and leaf and len(seen) < 200:
        a = stack.pop()
        if a in seen:
            continue
        seen.add(a)
        for c in callees(a):
            if c == -1:
                leaf = False
            elif is_stub(c) or c in handled:
                continue
            else:
                stack.append(c)
    leaf = leaf and len(seen) < 200
    print(f"{t:#x} {'leaf' if leaf else 'NOT-LEAF'} closure={len(seen)}")
    if leaf:
        leaf_all |= seen
json.dump(sorted(leaf_all), open("leaf_functions.json", "w"))
