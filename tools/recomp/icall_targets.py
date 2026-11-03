# Names the libil2cpp icall implementations that generated code calls directly
# (icalls are bound at build time through libil2cpp.icalls, so the binary has
# no name table): each call target is matched to the icall entries of the
# managed methods calling it.
#
# The managed method bound to the icall is a trampoline (a lone tail call);
# other callers reach the icall through inlining, so trampolines decide.
#
# usage: python3 -I icall_targets.py <libil2cpp.icalls> <script.json> <function_table.json> <out.json> <recomp dirs...>
import re, sys, glob, json, collections

icalls_path, script_path, table_path, out_path = sys.argv[1:5]
dirs = sys.argv[5:]
sizes = {f["addr"]: f["size"] for f in json.load(open(table_path))["functions"]}

icalls = collections.defaultdict(list)
for line in open(icalls_path):
    line = line.strip()
    if not line or line[0] in "/>":
        continue
    managed, cpp = line.rsplit(" ", 1)
    cls, rest = managed.split("::", 1)
    icalls[(cls, rest.split("(")[0])].append({"managed": managed, "cpp": cpp})

sigs = {}
for m in json.load(open(script_path))["ScriptMethod"]:
    sigs.setdefault(m["Address"], m["Signature"])

IL_KINDS = {"System.Boolean": "i8", "System.Byte": "i8", "System.SByte": "i8", "System.Int16": "i16", "System.UInt16": "i16",
            "System.Char": "i16", "System.Int32": "i32", "System.UInt32": "i32", "System.Int64": "i64", "System.UInt64": "i64",
            "System.IntPtr": "i64", "System.UIntPtr": "i64", "System.Single": "f32", "System.Double": "f64"}
C_KINDS = {"bool": "i8", "uint8_t": "i8", "int8_t": "i8", "int16_t": "i16", "uint16_t": "i16", "Il2CppChar": "i16",
           "int32_t": "i32", "uint32_t": "i32", "int64_t": "i64", "uint64_t": "i64", "intptr_t": "i64", "uintptr_t": "i64",
           "float": "f32", "double": "f64"}


def il_kinds(managed):
    params = managed[managed.index("(") + 1:managed.rindex(")")]
    out = []
    for t in filter(None, (x.strip() for x in params.split(","))):
        if t.endswith("&") or t.endswith("*"):
            out.append("ptr")
        elif t.endswith("[]"):
            out.append("arr")
        elif t == "System.String":
            out.append("str")
        else:
            out.append(IL_KINDS.get(t, "?"))
    return out


def c_kinds(signature):
    m = re.match(r"\s*(.+?)\s+\w+\s*\((.*)\);\s*$", signature)
    if not m:
        return None
    out = []
    for p in filter(None, (x.strip() for x in m.group(2).split(","))):
        if " " not in p:
            return None
        t, name = p.rsplit(" ", 1)
        if t.startswith("const MethodInfo"):
            continue
        if name == "__this":
            continue
        if t.endswith("**"):
            out.append("ptr")
        elif t == "System_String_o*":
            out.append("str")
        elif t.endswith("_array*"):
            out.append("arr")
        elif t.endswith("_o*") or t == "Il2CppObject*":
            out.append("obj")
        elif t.endswith("*"):
            out.append("ptr")
        else:
            out.append(C_KINDS.get(t, "?"))
    return out


def compatible(a, b):
    return len(a) == len(b) and all(x == y or "?" in (x, y) or {x, y} == {"obj", "?"} for x, y in zip(a, b))


def exactness(a, b):
    return sum(x == y for x, y in zip(a, b))


def in_runtime(a):
    return 0xfcbe4 <= a < 0x1252b8 or 0x125314 < a < 0x192c50


callers = collections.defaultdict(collections.Counter)
cur = None
for d in dirs:
    for p in glob.glob(f"{d}/recomp_*.cpp"):
        for line in open(p):
            if line.startswith("// "):
                name = line[3:].strip()
            elif line.startswith("void f_"):
                cur = (name, int(line[7:line.index("(")], 16))
            for m in re.finditer(r"CallImport\(c, (0x[0-9a-f]+)\)", line):
                a = int(m.group(1), 16)
                if in_runtime(a):
                    callers[a][cur] += 1

out = {}
for a, names in sorted(callers.items()):
    cands = {}
    trampolines = [(n, a) for n, a in names if sizes.get(a, 1 << 30) <= 16 and "$$" in n]
    for name, addr in (trampolines or names):
        if "$$" not in name:
            continue
        cls, meth = name.split("$$", 1)
        entries = icalls.get((cls, meth), [])
        ck = c_kinds(sigs.get(addr, ""))
        if ck is not None and len(entries) > 1:
            entries = [e for e in entries if compatible(il_kinds(e["managed"]), ck)] or entries
            best = max(exactness(il_kinds(e["managed"]), ck) for e in entries)
            entries = [e for e in entries if exactness(il_kinds(e["managed"]), ck) == best]
        for e in entries:
            cands[e["managed"]] = dict(e, caller=name, caller_signature=sigs.get(addr, ""))
    if len(cands) > 1:
        # several wrappers reached this target: keep the entries every exact wrapper agrees on
        votes = collections.Counter()
        for name, addr in names:
            ck = c_kinds(sigs.get(addr, ""))
            if ck is not None and "$$" in name:
                for m, e in cands.items():
                    if e["caller"] == name and compatible(il_kinds(m), ck):
                        votes[m] += 1
        if votes:
            best = max(votes.values())
            cands = {m: e for m, e in cands.items() if votes[m] == best}
    out[f"{a:#x}"] = {"callers": [n for (n, _), _ in names.most_common(5)], "candidates": list(cands.values())}
json.dump(out, open(out_path, "w"), indent=1)
print(f"{len(out)} targets: {sum(len(v['candidates']) == 1 for v in out.values())} unique, "
      f"{sum(len(v['candidates']) > 1 for v in out.values())} ambiguous, {sum(not v['candidates'] for v in out.values())} unnamed")
