# Enumerates every code pointer reachable from the game's IL2CPP registration
# tables (Il2CppCodeRegistration and its Il2CppCodeGenModules), i.e. every
# entry point native code may call into the game.
#
# usage: python3 -I registration_tables.py <GameAssembly arm64> <CodeRegistration hex> <out.json> [MetadataRegistration hex]
#
# Writes {"entries": [{"addr", "kind", "index", "module"}], "extra_functions": [...]}
# where extra_functions are targets outside the il2cpp section that the
# recompiler must translate too.
import sys, json, struct, collections
import lief

binary, code_reg, out_path = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
b = lief.parse(binary)
data = open(binary, "rb").read()
segments = [(s.virtual_address, s.virtual_size, s.file_offset, s.file_size) for s in b.segments]
gen = b.get_section("il2cpp")
text = b.get_section("__text")

def file_offset(va):
    for vaddr, vsize, foff, fsize in segments:
        if vaddr <= va < vaddr + fsize:
            return foff + (va - vaddr)
    raise ValueError(f"address {va:#x} not file-backed")

def u32(va): return struct.unpack_from("<I", data, file_offset(va))[0]
def u64(va): return struct.unpack_from("<Q", data, file_offset(va))[0]
def ptr_array(va, count): return [(u64(va + 8 * i), va + 8 * i) for i in range(count)] if va and count else []

def cstring(va):
    off = file_offset(va)
    return data[off:data.index(b"\0", off)].decode()

entries = []
def add(kind, pairs, module=None):
    for i, (a, slot) in enumerate(pairs):
        if a:
            entries.append({"addr": a, "slot": slot, "kind": kind, "index": i, "module": module})

# Il2CppCodeRegistration (Unity 2020.3, metadata v27): 8-byte aligned fields.
r = code_reg
reverse_count, reverse = u32(r + 0x00), u64(r + 0x08)
generic_count, generic, adjustors = u32(r + 0x10), u64(r + 0x18), u64(r + 0x20)
invoker_count, invokers = u32(r + 0x28), u64(r + 0x30)
unresolved_count, unresolved = u32(r + 0x38), u64(r + 0x40)
interop_count, interop = u32(r + 0x48), u64(r + 0x50)
winrt_count, winrt = u32(r + 0x58), u64(r + 0x60)
module_count, modules = u32(r + 0x68), u64(r + 0x70)

add("reverse_pinvoke_wrapper", ptr_array(reverse, reverse_count))
add("generic_method", ptr_array(generic, generic_count))
def code_pointer_run(va, limit):
    # Arrays without a stored length: read until a pointer leaves the code.
    out = []
    for i in range(limit):
        a = u64(va + 8 * i)
        if not (text.virtual_address <= a < gen.virtual_address + gen.size):
            break
        out.append((a, va + 8 * i))
    return out

# genericAdjustorThunks is indexed by adjustorThunkIndex, not by generic method.
add("generic_adjustor_thunk", code_pointer_run(adjustors, generic_count) if adjustors else [])
add("invoker", ptr_array(invokers, invoker_count))
add("unresolved_virtual_call", ptr_array(unresolved, unresolved_count))
# Il2CppInteropData: 5 function pointers, guid*, type*
for i in range(interop_count):
    base = interop + i * 56
    for j, kind in enumerate(["delegate_pinvoke_wrapper", "marshal_to_native", "marshal_from_native",
                              "marshal_cleanup", "create_ccw"]):
        a = u64(base + 8 * j)
        if a:
            entries.append({"addr": a, "slot": base + 8 * j, "kind": kind, "index": i, "module": None})
for i in range(winrt_count):
    a = u64(winrt + i * 16 + 8)
    if a:
        entries.append({"addr": a, "slot": winrt + i * 16 + 8, "kind": "winrt_factory", "index": i, "module": None})

# Il2CppCodeGenModule
for m, _ in ptr_array(modules, module_count):
    name = cstring(u64(m + 0x00))
    method_count, methods = u32(m + 0x08), u64(m + 0x10)
    adjustor_count, adjustor_pairs = u32(m + 0x18), u64(m + 0x20)
    attr_generators = u64(m + 0x68)
    module_initializer = u64(m + 0x70)
    add("method", ptr_array(methods, method_count), name)
    for i in range(adjustor_count):
        a = u64(adjustor_pairs + i * 16 + 8)
        token = u32(adjustor_pairs + i * 16)
        rid = token & 0xFFFFFF
        sig_addr = u64(methods + 8 * (rid - 1)) if 0 < rid <= method_count else 0
        if a:
            entries.append({"addr": a, "slot": adjustor_pairs + i * 16 + 8, "kind": "adjustor_thunk", "index": i,
                            "module": name, "sig_addr": sig_addr})
    if attr_generators:
        add("custom_attribute_generator", code_pointer_run(attr_generators, 1 << 20), name)
    if module_initializer:
        entries.append({"addr": module_initializer, "slot": m + 0x70, "kind": "module_initializer", "index": 0, "module": name})

if len(sys.argv) > 4:
    meta = int(sys.argv[4], 16)
    table_count, table = u32(meta + 0x20), u64(meta + 0x28)
    adjustor_to_method = {}
    for i in range(table_count):
        e = table + 16 * i
        method_index, adjustor_index = struct.unpack_from("<i", data, file_offset(e + 4))[0], struct.unpack_from("<i", data, file_offset(e + 12))[0]
        if adjustor_index >= 0:
            adjustor_to_method.setdefault(adjustor_index, method_index)
    generic_pointers = [u64(generic + 8 * k) for k in range(generic_count)]
    for e in entries:
        if e["kind"] == "generic_adjustor_thunk":
            mi = adjustor_to_method.get(e["index"])
            e["sig_addr"] = generic_pointers[mi] if mi is not None and 0 <= mi < generic_count else 0

in_gen = lambda a: gen.virtual_address <= a < gen.virtual_address + gen.size
in_text = lambda a: text.virtual_address <= a < text.virtual_address + text.size
summary = collections.Counter()
extra = set()
for e in entries:
    where = "il2cpp" if in_gen(e["addr"]) else "__text" if in_text(e["addr"]) else "other"
    e["section"] = where
    summary[(e["kind"], where)] += 1
    if where == "__text":
        extra.add(e["addr"])

json.dump({"entries": entries, "extra_functions": sorted(extra)}, open(out_path, "w"))
print(f"modules: {module_count}, entries: {len(entries)}, distinct targets: {len({e['addr'] for e in entries})}")
for (kind, where), n in sorted(summary.items()):
    print(f"  {kind:28s} {where:7s} {n:7d}")
print(f"extra functions outside the il2cpp section: {len(extra)}")
