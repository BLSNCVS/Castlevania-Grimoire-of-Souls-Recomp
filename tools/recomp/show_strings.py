# Shows C strings referenced by a function and its direct callees (depth N).
# usage: python3 -I show_strings.py <GameAssembly arm64> <addr hex>... [--depth N]
import sys, bisect, array
import lief, capstone
args = [a for a in sys.argv[2:] if not a.startswith("--")]
depth = int(sys.argv[sys.argv.index("--depth") + 1]) if "--depth" in sys.argv else 2
if "--depth" in sys.argv:
    args.remove(sys.argv[sys.argv.index("--depth") + 1])
b = lief.parse(sys.argv[1])
text = b.get_section("__text"); tva, td = text.virtual_address, bytes(text.content)
cs = b.get_section("__cstring"); cva, cd = cs.virtual_address, bytes(cs.content)
starts = sorted(a for a in b.function_starts.functions if tva <= a < tva + len(td))
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = True

def body(a):
    i = bisect.bisect_left(starts, a)
    end = starts[i + 1] if i + 1 < len(starts) else tva + len(td)
    return list(md.disasm(td[a - tva:end - tva], a))

def info(a):
    strings, calls, pages = [], [], {}
    for ins in body(a):
        if ins.mnemonic == "adrp":
            pages[ins.operands[0].reg] = ins.operands[1].imm
        elif ins.mnemonic == "add" and len(ins.operands) == 3 and ins.operands[1].reg in pages and ins.operands[2].type == capstone.arm64.ARM64_OP_IMM:
            v = pages[ins.operands[1].reg] + ins.operands[2].imm
            if cva <= v < cva + len(cd):
                strings.append(cd[v - cva:cd.index(b"\0", v - cva)].decode(errors="replace"))
        elif ins.mnemonic in ("bl", "b") and ins.operands[0].type == capstone.arm64.ARM64_OP_IMM:
            t = ins.operands[0].imm
            if t in set(starts) and t != a:
                calls.append(t)
    return strings, calls

for a in args:
    a = int(a, 16)
    seen, frontier, found = {a}, [a], []
    for level in range(depth + 1):
        nxt = []
        for f in frontier:
            s, c = info(f)
            found += [(level, f, x) for x in s]
            for t in c:
                if t not in seen:
                    seen.add(t); nxt.append(t)
        frontier = nxt
    print(f"{a:#x}: " + " | ".join(f"L{l}:{x[:40]}" for l, f, x in found[:12]))
