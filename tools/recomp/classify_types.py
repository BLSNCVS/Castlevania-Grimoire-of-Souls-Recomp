# Classifies how value types are passed and returned under AAPCS64 (the game's
# arm64 ABI) and their size/alignment, by letting clang lower functions that
# take and return each type, using Il2CppDumper's il2cpp.h.
#
# usage: python3 -I classify_types.py <il2cpp.h> <script.json> <out.json>
#
# Output per type: {"size", "align", "arg": <class>, "ret": <class>} where class is
#   {"kind": "hfa", "elem": "float"|"double", "count": n}
#   {"kind": "int", "regs": n}         (composite in 1-2 general registers)
#   {"kind": "indirect"}               (copied to memory, pointer passed / x8 for returns)
import sys, json, re, subprocess, os, tempfile

header, script, out_path = sys.argv[1:4]

PRIMITIVES = {"void", "int32_t", "uint32_t", "int64_t", "uint64_t", "float", "double", "bool", "uint8_t",
              "int8_t", "int16_t", "uint16_t", "Il2CppChar", "intptr_t", "uintptr_t"}

def split(sig):
    m = re.match(r"\s*(.+?)\s+\w+\s*\((.*)\);\s*$", sig)
    ret = m.group(1).strip()
    ps = [p.strip().rsplit(" ", 1)[0].strip() for p in m.group(2).split(",") if p.strip()]
    return ret, ps

types = set()
for m in json.load(open(script))["ScriptMethod"]:
    ret, ps = split(m["Signature"])
    for t in [ret] + ps:
        if not t.endswith("*") and t not in PRIMITIVES:
            types.add(t)
types = sorted(types)

with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, "classify.cpp")
    with open(src, "w") as f:
        f.write(f'#include <stdint.h>\n#include <stddef.h>\n#include "{os.path.abspath(header)}"\n')
        for i, t in enumerate(types):
            f.write(f'extern "C" void arg_{i}({t} a) {{}}\n')
            f.write(f'extern "C" {t} ret_{i}() {{ {t} r{{}}; return r; }}\n')
            f.write(f'extern "C" const size_t size_{i} = sizeof({t});\n')
            f.write(f'extern "C" const size_t align_{i} = alignof({t});\n')
    ir = os.path.join(tmp, "classify.ll")
    subprocess.run(["clang++", "--target=aarch64-linux-gnu", "--sysroot=/usr/aarch64-linux-gnu", "-x", "c++",
                    "-O1", "-S", "-emit-llvm", "-w", src, "-o", ir], check=True)
    text = open(ir).read()

def classify(irtype):
    irtype = irtype.strip()
    m = re.match(r"\[(\d+) x (float|double)\]$", irtype)
    if m:
        return {"kind": "hfa", "elem": m.group(2), "count": int(m.group(1))}
    if irtype in ("float", "double"):
        return {"kind": "hfa", "elem": irtype, "count": 1}
    m = re.match(r"\[(\d+) x i64\]$", irtype)
    if m:
        return {"kind": "int", "regs": int(m.group(1))}
    if re.match(r"i\d+$", irtype):
        return {"kind": "int", "regs": 1}
    if irtype.startswith("ptr"):
        return {"kind": "indirect"}
    if irtype == "void":
        return {"kind": "empty"}
    return {"kind": "unknown", "ir": irtype}

result = {}
consts = dict(re.findall(r"@((?:size|align)_\d+) = (?:dso_local )?(?:local_unnamed_addr )?constant i64 (\d+)", text))
for i, t in enumerate(types):
    arg = re.search(rf"define[^@\n]*@arg_{i}\((.*)\)[^)\n]*\{{", text)
    ret = re.search(rf"define[^@]*?(\S+(?: \[[^\]]*\])?) @ret_{i}\(([^)]*)\)", text)
    entry = {"size": int(consts.get(f"size_{i}", 0)), "align": int(consts.get(f"align_{i}", 0))}
    a = arg.group(1).strip() if arg else ""
    a = re.sub(r"\s+%\S+$", "", a)
    a = re.sub(r"\s+(noundef|nocapture|readonly|readnone|dead_on_unwind|writable|alignstack\(\d+\)|byval\([^)]*\)|align \d+)", "", a).strip()
    entry["arg"] = classify(a if a else "void")
    if ret:
        params = ret.group(2)
        if "sret" in params:
            entry["ret"] = {"kind": "indirect"}
        else:
            r = re.search(rf"define\s+(?:dso_local\s+)?(.+?)\s+@ret_{i}\(", text).group(1)
            r = re.sub(r"\b(local_unnamed_addr|noundef|range\([^)]*\))\s*", "", r).strip()
            if r.startswith("%struct."):
                # clang returns HFAs as the named struct; same registers as the argument
                entry["ret"] = entry["arg"]
            else:
                entry["ret"] = classify(r)
    result[t] = entry

json.dump(result, open(out_path, "w"), indent=0)
kinds = {}
for e in result.values():
    k = e["arg"]["kind"]
    kinds[k] = kinds.get(k, 0) + 1
print(f"types: {len(result)}  argument classes: {kinds}")
unknown = [t for t, e in result.items() if e["arg"]["kind"] == "unknown" or e.get("ret", {}).get("kind") == "unknown"]
print("unknown:", unknown[:10])
for t in ["UnityEngine_Vector3_o", "UnityEngine_Vector2_o", "System_DateTime_o", "UnityEngine_Matrix4x4_o",
          "System_Decimal_o", "UnityEngine_Color_o", "System_Collections_Generic_KeyValuePair_TKey__TValue__o"]:
    if t in result:
        print(f"  {t}: {result[t]}")
