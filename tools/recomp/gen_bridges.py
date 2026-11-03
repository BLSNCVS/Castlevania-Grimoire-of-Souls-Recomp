# Generates native entry thunks for every code pointer in the game's IL2CPP
# registration tables (see registration_tables.py), following the arm64
# argument assignment (AAPCS64, Apple variant) on the guest side.
#
# usage: python3 -I gen_bridges.py <registration.json> <script.json> <type_classes.json> <out_dir>
#            [--only-functions functions.txt]
import sys, json, re, os, argparse, collections

ap = argparse.ArgumentParser()
ap.add_argument("registration")
ap.add_argument("script")
ap.add_argument("types")
ap.add_argument("out_dir")
ap.add_argument("--only-functions", help="restrict to entries whose target is in this list")
args = ap.parse_args()

INTS = {"int32_t": ("int32_t", True), "uint32_t": ("uint32_t", False), "int64_t": ("int64_t", True),
        "uint64_t": ("uint64_t", False), "bool": ("bool", False), "uint8_t": ("uint8_t", False),
        "int8_t": ("int8_t", True), "int16_t": ("int16_t", True), "uint16_t": ("uint16_t", False),
        "Il2CppChar": ("uint16_t", False), "intptr_t": ("int64_t", True), "uintptr_t": ("uint64_t", False)}
FLOATS = {"float", "double"}
types = json.load(open(args.types))

def split(sig):
    m = re.match(r"\s*(.+?)\s+\w+\s*\((.*)\);\s*$", sig)
    ret = m.group(1).strip()
    ps = [p.strip().rsplit(" ", 1)[0].strip() for p in m.group(2).split(",") if p.strip()]
    return ret, ps

def normalize(t):
    """Native-side type key: P (pointer), int types, float/double, S<size>_<align>:<class>."""
    if t.endswith("*"):
        return "P"
    if t in INTS or t in FLOATS or t == "void":
        return t
    info = types[t]
    cls = info["arg"]
    tag = cls["kind"] if cls["kind"] != "hfa" else f"hfa{cls['count']}{cls['elem'][0]}"
    if cls["kind"] == "int":
        tag = f"int{cls['regs']}"
    return f"S{info['size']}_{info['align']}_{tag}"

def native_type(key):
    if key == "P":
        return "void*"
    if key in INTS:
        return INTS[key][0]
    if key in FLOATS or key == "void":
        return key
    size, align = key[1:].split("_")[:2]
    return f"Blob<{size}, {align}>"

class Packer:
    """Emits code assigning native arguments to the guest Ctx (Apple arm64)."""

    def __init__(self):
        self.ngrn = 0
        self.nsrn = 0
        self.stack = 0
        self.lines = []

    def stack_slot(self, size, align):
        self.stack = (self.stack + align - 1) // align * align
        off = self.stack
        self.stack += size
        return off

    def add(self, key, name):
        L = self.lines
        if key == "P" or key in INTS:
            if key == "P":
                expr = f"uint64_t({name})"
            else:
                t, signed = INTS[key]
                expr = f"uint64_t(int64_t({name}))" if signed else f"uint64_t({name})"
            if self.ngrn < 8:
                L.append(f"c.x[{self.ngrn}] = {expr};")
                self.ngrn += 1
            else:
                size = 8 if key in ("P", "int64_t", "uint64_t", "intptr_t", "uintptr_t") else 4 if key in ("int32_t", "uint32_t") else 2 if key in ("int16_t", "uint16_t", "Il2CppChar") else 1
                off = self.stack_slot(size, size)
                L.append(f"memcpy(stackArgs + {off}, &{name}, {size});")
            return
        if key in FLOATS:
            if self.nsrn < 8:
                L.append(f"{'SetS' if key == 'float' else 'SetD'}(c.v[{self.nsrn}], {name});")
                self.nsrn += 1
            else:
                size = 4 if key == "float" else 8
                off = self.stack_slot(size, size)
                L.append(f"memcpy(stackArgs + {off}, &{name}, {size});")
            return
        size, align, cls = key[1:].split("_", 2)
        size, align = int(size), int(align)
        if cls.startswith("hfa"):
            n, elem = int(cls[3]), cls[4]
            esize = 4 if elem == "f" else 8
            if self.nsrn + n <= 8:
                for k in range(n):
                    setter = "SetS32" if esize == 4 else "SetD64"
                    ctype = "uint32_t" if esize == 4 else "uint64_t"
                    L.append(f"{{ {ctype} e; memcpy(&e, {name}.bytes + {k * esize}, {esize}); {setter}(c.v[{self.nsrn + k}], e); }}")
                self.nsrn += n
            else:
                self.nsrn = 8
                off = self.stack_slot(size, max(align, 8))
                L.append(f"memcpy(stackArgs + {off}, {name}.bytes, {size});")
            return
        if cls.startswith("int"):
            regs = int(cls[3])
            if self.ngrn + regs <= 8:
                L.append(f"CopyToRegs(&c.x[{self.ngrn}], {name});")
                self.ngrn += regs
            else:
                self.ngrn = 8
                off = self.stack_slot(size, max(align, 8))
                L.append(f"memcpy(stackArgs + {off}, {name}.bytes, {size});")
            return
        # indirect: caller-owned copy, pointer passed as an integer argument
        L.append(f"alignas(16) uint8_t copy_{name}[{size}]; memcpy(copy_{name}, {name}.bytes, {size});")
        self.add("P", f"copy_{name}")

def return_code(key):
    """Returns (setup lines before the call, expression producing the native result)."""
    if key == "void":
        return [], None
    if key == "P":
        return [], "reinterpret_cast<void*>(c.x[0])"
    if key in INTS:
        return [], f"{INTS[key][0]}(c.x[0])"
    if key == "float":
        return [], "c.v[0].f32[0]"
    if key == "double":
        return [], "c.v[0].f64[0]"
    size, align, cls = key[1:].split("_", 2)
    t = native_type(key)
    if cls.startswith("hfa"):
        n, elem = int(cls[3]), cls[4]
        esize = 4 if elem == "f" else 8
        lane = "u32" if esize == 4 else "u64"
        parts = " ".join(f"memcpy(r.bytes + {k * esize}, &c.v[{k}].{lane}[0], {esize});" for k in range(n))
        return [], f"[&] {{ {t} r; {parts} return r; }}()"
    if cls.startswith("int"):
        return [], f"CopyFromRegs<{t}>(&c.x[0])"
    return [f"alignas(16) {t} result; c.x[8] = reinterpret_cast<uint64_t>(&result);"], "result"

FIXED = {
    "invoker": ("P", ["P", "P", "P", "P"]),
    "custom_attribute_generator": ("void", ["P"]),
    "marshal_to_native": ("void", ["P", "P"]),
    "marshal_from_native": ("void", ["P", "P"]),
    "marshal_cleanup": ("void", ["P"]),
    "module_initializer": ("void", []),
}
# Signature unknown: forward four integer arguments (enough for the stubs
# that only raise exceptions, and for wrappers taking pointers).
FALLBACK = ("uint64_t", ["uint64_t", "uint64_t", "uint64_t", "uint64_t"])

script_sig = {}
for m in json.load(open(args.script))["ScriptMethod"]:
    script_sig.setdefault(m["Address"], m["Signature"])

entries = json.load(open(args.registration))["entries"]
if args.only_functions:
    keep = {int(l, 16) for l in open(args.only_functions) if l.strip()}
    entries = [e for e in entries if e["addr"] in keep]

classes = {}
table = []
fallback_count = collections.Counter()
for e in entries:
    if e["kind"] in FIXED:
        ret, params = FIXED[e["kind"]]
    else:
        sig_addr = e.get("sig_addr", e["addr"])
        if sig_addr in script_sig:
            r, ps = split(script_sig[sig_addr])
            ret, params = normalize(r), [normalize(p) for p in ps]
        else:
            ret, params = FALLBACK
            fallback_count[e["kind"]] += 1
    key = (ret, tuple(params))
    cid = classes.setdefault(key, len(classes))
    table.append((e["slot"], cid, e["addr"]))

os.makedirs(args.out_dir, exist_ok=True)
with open(os.path.join(args.out_dir, "bridges_entry.h"), "w") as out:
    out.write("#pragma once\n// Generated by tools/recomp/gen_bridges.py\n#include <cstring>\n")
    out.write('#include "recomp_bridge.h"\n\nnamespace recomp\n{\n')
    for (ret, params), cid in classes.items():
        p = Packer()
        names = [f"a{i}" for i in range(len(params))]
        for key, name in zip(params, names):
            p.add(key, name)
        setup, result = return_code(ret)
        plist = ", ".join(f"{native_type(k)} {n}" for k, n in zip(params, names))
        out.write(f"template <Function F> {native_type(ret)} Entry{cid}({plist})\n{{\n")
        out.write("    GuestStackScope scope;\n    Ctx c;\n    memset(&c, 0, sizeof(c));\n")
        stack_bytes = (p.stack + 15) // 16 * 16
        out.write(f"    c.sp = scope.Top() - {stack_bytes};\n")
        if p.stack:
            out.write("    uint8_t* stackArgs = reinterpret_cast<uint8_t*>(c.sp);\n")
        for line in p.lines:
            out.write(f"    {line}\n")
        for line in setup:
            out.write(f"    {line}\n")
        out.write("    F(c);\n")
        if result is not None:
            out.write(f"    return {result};\n")
        out.write("}\n\n")
    out.write("}\n")

# The table is split so no single file grows too large to compile quickly.
chunk = 20000
names = []
for n in range(0, len(table), chunk):
    name = f"bridges_table_{n // chunk:03d}.cpp"
    names.append(name)
    with open(os.path.join(args.out_dir, name), "w") as out:
        out.write('// Generated by tools/recomp/gen_bridges.py\n#include "bridges_entry.h"\n#include "recomp_decls.h"\n\n')
        out.write(f"namespace recomp\n{{\nextern const BridgeEntry kBridgeTablePart{n // chunk}[] = {{\n")
        for slot, cid, addr in table[n:n + chunk]:
            out.write(f"    {{ {slot:#x}, reinterpret_cast<void*>(&Entry{cid}<f_{addr:x}>), f_{addr:x} }},\n")
        out.write("};\n}\n")
with open(os.path.join(args.out_dir, "bridges_table.cpp"), "w") as out:
    out.write('// Generated by tools/recomp/gen_bridges.py\n#include "recomp_bridge.h"\n\nnamespace recomp\n{\n')
    parts = (len(table) + chunk - 1) // chunk
    for k in range(parts):
        out.write(f"extern const BridgeEntry kBridgeTablePart{k}[];\n")
    out.write("struct BridgePart { const BridgeEntry* entries; size_t count; };\n")
    out.write("extern const BridgePart kBridgeParts[] = {\n")
    for k in range(parts):
        count = min(chunk, len(table) - k * chunk)
        out.write(f"    {{ kBridgeTablePart{k}, {count} }},\n")
    out.write(f"}};\nextern const size_t kBridgePartCount = {parts};\n}}\n")

print(f"entries: {len(table)}, thunk classes: {len(classes)}, table files: {len(names)}")
print(f"entries using the fallback signature: {dict(fallback_count)}")
