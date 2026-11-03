# Lists call sites (bl/b) of target addresses in the il2cpp section, with the
# instructions just before each call for argument inspection.
# usage: python3 -I callers.py <UnityFramework> <function_table.json> <hexaddr>...
import sys, json, bisect, array, subprocess
import lief
b = lief.parse(sys.argv[1]); tg = {int(a, 16) for a in sys.argv[3:]}
fns = json.load(open(sys.argv[2]))["functions"]; starts = [f["addr"] for f in fns]
g = b.get_section("il2cpp"); words = array.array("I", bytes(g.content)); base = g.virtual_address
for idx, ins in enumerate(words):
    if ins & 0x7C000000 == 0x14000000:  # b / bl
        off = ins & 0x3FFFFFF
        if off & (1 << 25): off -= 1 << 26
        pc = base + idx * 4; t = pc + off * 4
        if t in tg:
            f = fns[bisect.bisect_right(starts, pc) - 1]
            print(f"== {hex(t)} <- {hex(pc)} {f['name']}")
            out = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", f"--start-address={pc-24}", f"--stop-address={pc+4}", sys.argv[1]], capture_output=True, text=True).stdout
            print("\n".join(l for l in out.splitlines() if l.strip().startswith(tuple("0123456789abcdef")) and ":" in l))
