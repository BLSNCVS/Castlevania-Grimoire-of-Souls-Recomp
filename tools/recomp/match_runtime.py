# Names the libil2cpp runtime functions in the game's GameAssembly (stripped)
# by matching them against a libil2cpp built from source with symbols.
#
# Matching starts from the exported il2cpp_* API (named in both), walks direct
# calls in order, and pairs callees whose instruction fingerprints agree.
#
# usage: python3 -I match_runtime.py <GameAssembly arm64> <function_table.json> <objdir> <out.json>
#   objdir: libil2cpp compiled for arm64 (*.o with symbols)
import sys, json, glob, subprocess, re, collections, difflib, bisect
import lief, capstone

binary, table_path, objdir, out_path = sys.argv[1:5]

# ---- game side -------------------------------------------------------------
b = lief.parse(binary)
text = b.get_section("__text")
tva, tdata = text.virtual_address, bytes(text.content)
starts = sorted(a for a in b.function_starts.functions if tva <= a < tva + len(tdata))
exports = {s.name.lstrip("_"): s.value for s in b.exported_symbols if s.value}
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

game = {}
for i, a in enumerate(starts):
    end = starts[i + 1] if i + 1 < len(starts) else tva + len(tdata)
    mnems, calls = [], []
    for (addr, size, mn, op) in md.disasm_lite(tdata[a - tva:end - tva], a):
        mnems.append(mn)
        if mn in ("bl", "b") and op.startswith("#"):
            t = int(op[1:], 16)
            if not (a <= t < end):
                calls.append(t)
    game[a] = (mnems, calls)

# ---- source build side -------------------------------------------------------
ours = {}
object_order = []  # per object file: function symbols in .text order
sym_re = re.compile(r"^[0-9a-f]+ <(.+)>:$")
for obj in sorted(glob.glob(objdir + "/*.o")):
    out = subprocess.run(["llvm-objdump", "-d", "-r", "--no-show-raw-insn", obj], capture_output=True, text=True).stdout
    cur = None
    order = []
    for line in out.splitlines():
        if line.startswith("Disassembly of section"):
            in_text = line.rstrip(":").endswith(".text")
            cur = None
            continue
        m = sym_re.match(line)
        if m:
            cur = m.group(1)
            ours[cur] = ([], [])
            if in_text and not cur.startswith("$"):
                order.append(cur)
            continue
        if cur is None:
            continue
        line = line.strip()
        if "R_AARCH64_CALL26" in line or "R_AARCH64_JUMP26" in line:
            target = line.split()[-1]
            target = re.sub(r"\+0x[0-9a-f]+$", "", target)
            ours[cur][1].append(target)
        elif re.match(r"^[0-9a-f]+:\s", line):
            parts = line.split(":", 1)[1].split()
            if parts:
                ours[cur][0].append(parts[0])
    object_order.append(order)

def demangle(names):
    out = subprocess.run(["llvm-cxxfilt"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out))

# ---- matching ----------------------------------------------------------------
def similarity(a, b):
    if not a or not b:
        return 0.0
    if abs(len(a) - len(b)) > max(len(a), len(b)) * 0.5 + 4:
        return 0.0
    return difflib.SequenceMatcher(None, a[:400], b[:400], autojunk=False).ratio()

matched = {}       # game address -> our symbol
used = set()
queue = collections.deque()

def accept(addr, name):
    if addr in matched or name in used or addr not in game or name not in ours:
        return False
    matched[addr] = name
    used.add(name)
    queue.append(addr)
    return True

for name, addr in exports.items():
    accept(addr, name)

# Exact instruction shape, unique on both sides (functions of 6+ instructions).
def shape(mnems):
    return tuple(mnems)

game_shapes = collections.defaultdict(list)
for a, (mn, _) in game.items():
    if len(mn) >= 6:
        game_shapes[shape(mn)].append(a)
our_shapes = collections.defaultdict(list)
for n, (mn, _) in ours.items():
    if len(mn) >= 6:
        our_shapes[shape(mn)].append(n)
exact = 0
for key, names_ in our_shapes.items():
    if len(names_) == 1 and len(game_shapes.get(key, [])) == 1:
        exact += accept(game_shapes[key][0], names_[0])
print(f"exact unique matches: {exact}")

# Source-file order: functions of one object file are contiguous and in order
# in the game binary. Between two matched functions of the same object, pair
# the remaining ones by order and shape (unused functions were dead-stripped).
start_index = {a: i for i, a in enumerate(starts)}
def short_key(mn):
    return (len(mn) if len(mn) < 8 else 8, tuple(mn[:4]), tuple(mn[-2:]))

order_matches = 0
for order in object_order:
    anchors = [(i, start_index[a]) for i, n in enumerate(order)
               for a in [next((g for g, nn in matched.items() if nn == n), None)] if a is not None]
    for (i1, g1), (i2, g2) in zip(anchors, anchors[1:]):
        if not (0 < g2 - g1 <= 4 * (i2 - i1) + 8) or i2 - i1 < 2:
            continue
        ours_mid = order[i1 + 1:i2]
        game_mid = starts[g1 + 1:g2]
        sm = difflib.SequenceMatcher(None, [short_key(ours[n][0]) for n in ours_mid],
                                     [short_key(game[a][0]) for a in game_mid], autojunk=False)
        for tag, a1, a2, b1, b2 in sm.get_opcodes():
            if tag == "equal":
                for n, a in zip(ours_mid[a1:a2], game_mid[b1:b2]):
                    order_matches += accept(a, n)
print(f"source-order matches: {order_matches}")

while queue:
    g = queue.popleft()
    gcalls = game[g][1]
    ocalls = ours[matched[g]][1]
    # align the two call sequences on callee fingerprints
    gkeys = [len(game[t][0]) // 4 if t in game else -1 for t in gcalls]
    okeys = [len(ours[t][0]) // 4 if t in ours else -1 for t in ocalls]
    sm = difflib.SequenceMatcher(None, gkeys, okeys, autojunk=False)
    pairs = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag in ("equal", "replace") and (i2 - i1) == (j2 - j1):
            pairs += zip(gcalls[i1:i2], ocalls[j1:j2])
    if len(gcalls) == len(ocalls):
        pairs += zip(gcalls, ocalls)
    for gt, ot in pairs:
        if gt in matched or ot in used or gt not in game or ot not in ours:
            continue
        if similarity(game[gt][0], ours[ot][0]) < 0.6:
            continue
        matched[gt] = ot
        used.add(ot)
        queue.append(gt)

targets = json.load(open(table_path))["imports"]
total_calls = sum(t["calls"] for t in targets)
named = [t for t in targets if t["addr"] in matched]
names = demangle([matched[t["addr"]] for t in named])
print(f"runtime functions matched overall: {len(matched)} of {len(game)}")
print(f"helper targets named: {len(named)} of {len(targets)}, covering "
      f"{100 * sum(t['calls'] for t in named) / total_calls:.2f}% of calls")
result = []
for t in sorted(targets, key=lambda t: -t["calls"]):
    sym = matched.get(t["addr"])
    result.append({"addr": t["addr"], "calls": t["calls"], "symbol": sym, "name": names.get(sym) if sym else None})
json.dump(result, open(out_path, "w"), indent=1)
for r in result[:40]:
    print(f"{r['addr']:#10x} {r['calls']:7d}  {r['name'] or '?'}")
