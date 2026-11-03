# Splits the game binary's __text into generated code (to recompile) and the
# libil2cpp runtime (replaced by the native Windows build).
#
# Generated objects are dense with functions the registration tables point to;
# runtime objects are long runs without any. Unreferenced runs shorter than
# --max-gap between generated functions count as generated (local helpers).
#
# usage: python3 -I classify_text.py <GameAssembly arm64> <registration.json>
#            <function_table.json> <out.json> [--max-gap N]
import sys, json, bisect, argparse
import lief, capstone

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("registration")
ap.add_argument("table")
ap.add_argument("out")
ap.add_argument("--max-gap", type=int, default=6)
args = ap.parse_args()

b = lief.parse(args.binary)
text = b.get_section("__text")
tva, td = text.virtual_address, bytes(text.content)
starts = sorted(a for a in b.function_starts.functions if tva <= a < tva + len(td))
referenced = set(json.load(open(args.registration))["extra_functions"])
helpers = {i["addr"] for i in json.load(open(args.table))["imports"]}

generated = [a in referenced for a in starts]
i = 0
n = len(starts)
while i < n:
    if generated[i]:
        i += 1
        continue
    j = i
    while j < n and not generated[j]:
        j += 1
    bounded = i > 0 and j < n
    if bounded and j - i <= args.max_gap and not any(starts[k] in helpers for k in range(i, j)):
        for k in range(i, j):
            generated[k] = True
    i = j

# Calls from generated code to unreferenced, non-helper __text functions:
# pull those in as generated too when they sit inside generated runs.
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
index = {a: k for k, a in enumerate(starts)}
gen_set = {starts[k] for k in range(n) if generated[k]}
runtime_targets = set()
for k in range(n):
    if not generated[k]:
        continue
    a = starts[k]
    end = starts[k + 1] if k + 1 < n else tva + len(td)
    for (addr, size, mn, op) in md.disasm_lite(td[a - tva:end - tva], a):
        if mn in ("bl", "b") and op.startswith("#"):
            t = int(op[1:], 16)
            if t in index and t not in gen_set and not (a <= t < end):
                runtime_targets.add(t)

result = {
    "generated": sorted(gen_set),
    "runtime_targets_from_generated": sorted(runtime_targets),
}
json.dump(result, open(args.out, "w"))
helpers_misclassified = len(helpers & gen_set)
print(f"__text functions: {n}, generated: {len(gen_set)}, runtime: {n - len(gen_set)}")
print(f"referenced by registration tables: {len(referenced)}")
print(f"known runtime helpers classified as generated: {helpers_misclassified}")
print(f"runtime functions called from generated code: {len(runtime_targets)} "
      f"(new beyond the il2cpp-section helper set: {len(runtime_targets - helpers)})")
