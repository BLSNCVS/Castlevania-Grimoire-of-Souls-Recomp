# Builds the recompiler's function table from an Il2CppDumper script.json and
# measures which addresses outside the generated code the game calls into.
#
# usage: python3 -I function_table.py <UnityFramework> <script.json> <out.json>
import sys, json, collections
import lief, capstone

binpath, scriptjson, out = sys.argv[1:4]

meths = {}
for m in json.load(open(scriptjson))["ScriptMethod"]:
    meths.setdefault(m["Address"], m["Name"])

b = lief.parse(binpath)
# Unity links all IL2CPP-generated code into its own "il2cpp" section; the
# engine and libil2cpp runtime live in __text.
gen = b.get_section("il2cpp")
gva, gdata = gen.virtual_address, bytes(gen.content)
eng = b.get_section("__text")
eva, esize = eng.virtual_address, eng.size
syms = {s.value: s.name for s in b.symbols if s.value}

addrs = sorted(a for a in meths if gva <= a < gva + len(gdata))
aset = set(addrs)

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
ext = collections.Counter()
table = []
for i, a in enumerate(addrs):
    # End is approximated by the next method start; CFG analysis refines it.
    end = addrs[i + 1] if i + 1 < len(addrs) else gva + len(gdata)
    table.append({"addr": a, "size": end - a, "name": meths[a]})
    off = a - gva
    for ins in md.disasm(gdata[off:off + (end - a)], a):
        if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
            t = int(ins.op_str[1:], 16)
            if t not in aset and not (a <= t < end):
                ext[t] += 1

imports = [{"addr": t, "calls": c, "symbol": syms.get(t, "")}
           for t, c in ext.most_common() if not (gva <= t < gva + len(gdata))]
json.dump({"functions": table, "imports": imports}, open(out, "w"))

print("functions:", len(table), "covering", sum(f["size"] for f in table), "of", len(gdata), "bytes")
print("distinct external targets:", len(imports),
      "(engine/runtime:", sum(1 for x in imports if eva <= x["addr"] < eva + esize), ")")
for x in imports[:25]:
    print(hex(x["addr"]), x["calls"], x["symbol"])
