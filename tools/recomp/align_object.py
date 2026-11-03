# Aligns the functions of one libil2cpp object file (built from source, with
# symbols) to a region of the game's stripped __text, where the linker kept the
# object's functions together and in source order (minus dead-stripped ones).
#
# usage: python3 -I align_object.py <GameAssembly arm64> <object.o> <region start hex> [count]
# Prints the alignment as JSON lines: {"addr", "symbol", "score"}.
import sys, json, re, subprocess, bisect, difflib
import lief, capstone

binary, obj, region_start = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
count = int(sys.argv[4]) if len(sys.argv) > 4 else 0

b = lief.parse(binary)
text = b.get_section("__text")
tva, td = text.virtual_address, bytes(text.content)
starts = sorted(a for a in b.function_starts.functions if tva <= a < tva + len(td))
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

def game_function(a):
    i = bisect.bisect_left(starts, a)
    if i >= len(starts) or starts[i] != a:
        return None
    end = starts[i + 1] if i + 1 < len(starts) else tva + len(td)
    mn, tail = [], None
    for (addr, size, m, op) in md.disasm_lite(td[a - tva:end - tva], a):
        mn.append(m)
        if m == "b" and op.startswith("#"):
            t = int(op[1:], 16)
            if not (a <= t < end):
                tail = t
    return mn, tail

out = subprocess.run(["llvm-objdump", "-d", "-r", "--no-show-raw-insn", obj], capture_output=True, text=True).stdout
ours, cur = [], None
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
    if m:
        cur = {"symbol": m.group(1), "mn": [], "tail": None}
        ours.append(cur)
        continue
    if cur is None:
        continue
    if "R_AARCH64_JUMP26" in line:
        cur["tail"] = re.sub(r"\+0x[0-9a-f]+$", "", line.split()[-1])
    elif re.match(r"^\s+[0-9a-f]+:\s", line):
        cur["mn"].append(line.split(":", 1)[1].split()[0])

# Fingerprints of our tail-call targets, from the whole build directory.
objdir = obj.rsplit("/", 1)[0]
target_names = {o["tail"] for o in ours if o["tail"]}
target_shapes = {}
if target_names:
    import glob
    for other in glob.glob(objdir + "/*.o"):
        dump = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", other], capture_output=True, text=True).stdout
        cur = None
        for line in dump.splitlines():
            m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
            if m:
                cur = m.group(1) if m.group(1) in target_names else None
                if cur:
                    target_shapes[cur] = []
                continue
            if cur and re.match(r"^\s+[0-9a-f]+:\s", line):
                target_shapes[cur].append(line.split(":", 1)[1].split()[0])

i0 = bisect.bisect_left(starts, region_start)
n_game = count or len(ours) + 40
game = []
for a in starts[i0:i0 + n_game]:
    mn, tail = game_function(a)
    game.append({"addr": a, "mn": mn, "tail": tail})

def shape_score(x, y):
    if not x or not y:
        return 0.0
    ratio = min(len(x), len(y)) / max(len(x), len(y))
    if ratio < 0.4:
        return 0.0
    return 0.5 * ratio + 0.5 * difflib.SequenceMatcher(None, x[:64], y[:64], autojunk=False).ratio()

def score(g, o):
    s = shape_score(g["mn"], o["mn"])
    # tail-call wrappers: compare what they forward to
    if g["tail"] is not None and o["tail"] and len(g["mn"]) <= 4 and len(o["mn"]) <= 4:
        target = game_function(g["tail"])
        if target and o["tail"] in target_shapes:
            s = 0.3 * s + 0.7 * shape_score(target[0], target_shapes[o["tail"]])
    return s

# Needleman-Wunsch: our functions may be dead-stripped (gap, small penalty);
# extra game functions (gap) cost more.
GAP_OURS, GAP_GAME, THRESHOLD = -0.1, -0.4, 0.45
n, m = len(ours), len(game)
dp = [[0.0] * (m + 1) for _ in range(n + 1)]
bt = [[0] * (m + 1) for _ in range(n + 1)]
for i in range(1, n + 1):
    dp[i][0] = dp[i - 1][0] + GAP_OURS
    bt[i][0] = 1
for j in range(1, m + 1):
    dp[0][j] = 0.0  # free leading game functions (region start may be early)
    bt[0][j] = 2
cache = {}
for i in range(1, n + 1):
    for j in range(1, m + 1):
        s = score(game[j - 1], ours[i - 1])
        cache[(i, j)] = s
        best, move = dp[i - 1][j - 1] + (s - THRESHOLD), 0
        if dp[i - 1][j] + GAP_OURS > best:
            best, move = dp[i - 1][j] + GAP_OURS, 1
        if dp[i][j - 1] + GAP_GAME > best:
            best, move = dp[i][j - 1] + GAP_GAME, 2
        dp[i][j], bt[i][j] = best, move

j = max(range(m + 1), key=lambda jj: dp[n][jj])
i = n
pairs = []
while i > 0 and j > 0:
    move = bt[i][j]
    if move == 0:
        pairs.append((game[j - 1]["addr"], ours[i - 1]["symbol"], cache[(i, j)]))
        i, j = i - 1, j - 1
    elif move == 1:
        i -= 1
    else:
        j -= 1
for addr, sym, s in reversed(pairs):
    print(json.dumps({"addr": addr, "symbol": sym, "score": round(s, 3)}))
