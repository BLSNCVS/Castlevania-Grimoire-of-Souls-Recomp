# Builds the table of functions to translate besides the registration-derived
# ones: compiler-outlined helpers of the generated code (NullCheck,
# InitializedTypeInfo, __clang_call_terminate, ...) that no registration table
# lists, plus self-contained leaf code of the runtime (Interlocked icalls,
# zlib's inflate internals, ...) that is simpler to translate than to map.
#
# Everything else outside the generated code is the runtime (libil2cpp,
# il2cpp-api, Boehm GC, baselib, cold assert paths), reached through
# CallImport handlers instead.
#
# usage: python3 -I gap_functions.py <GameAssembly arm64> <function_table.json> <leaf.json> <out.json>
import sys, json, bisect
import lief

binary, table_path, leaf_path, out_path = sys.argv[1:5]

# Runtime code in the macOS v1.4.2 arm64 GameAssembly, by link order.
RUNTIME = [
    (0x2670, 0x2928),      # baselib helpers (semaphores, timers, std::mutex)
    (0xfcbe4, 0xfecc0),    # il2cpp-api.cpp
    (0xff280, 0x1252b8),   # libil2cpp icalls, os layer, GC, zlib
    (0x125318, 0x1931a4),  # libil2cpp vm, metadata, codegen helpers
    (0x3f4458, 0x3fcc60),  # outlined cold paths (assertions)
]
TEXT_END = 0x3400214       # __stubs follows __text

b = lief.parse(binary)
starts = sorted(set(b.function_starts.functions))
known = {f["addr"] for f in json.load(open(table_path))["functions"]}
leaf = set(json.load(open(leaf_path))) if leaf_path != "-" else set()
leaf |= {a for a in starts if 0x187c98 <= a <= 0x187e18} | {0x187ff0, 0x125670, 0x125340}


def in_runtime(a):
    return any(lo <= a < hi for lo, hi in RUNTIME)


selected = sorted({a for a in starts if a not in known and not in_runtime(a) and a < TEXT_END} | (leaf - known))
bounds = sorted(set(starts) | known | {0x125314, 0x1931a4, TEXT_END})
out = [{"addr": a, "size": bounds[bisect.bisect_right(bounds, a)] - a, "name": f"gap_{a:x}"} for a in selected]
json.dump({"functions": out, "runtime": RUNTIME}, open(out_path, "w"))
print(f"{len(out)} functions ({len(leaf - known)} runtime leaf functions)")
