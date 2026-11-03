# Instruction-level differential test for the recompiler.
#
# Samples every distinct register-only instruction form in the game's code
# (data processing, floating point, SIMD; no memory or control flow), runs
# each sample in Unicorn from random register states, and writes a C++
# harness that runs the recompiler's translation of the same instruction from
# the same states and compares every register and the flags.
#
# usage: python3 -I insn_difftest.py <GameAssembly arm64> <out_dir> [--per-form N] [--runs N]
#   then build <out_dir>/harness.cpp with recomp/runtime on the include path and run it.
import argparse, collections, os, random, re, struct, sys

import capstone
import lief
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UcError
from unicorn.arm64_const import *

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arm64_recomp

CODE = 0x10000
MEM = 0x10000000          # data region for load/store forms (same address in the harness)
MEM_SIZE = 0x10000
MEM_SKIP = re.compile(r"^(b|bl|br|blr|ret|cb|tb|prfm|dmb|dsb|isb|svc|brk|hint|nop|adr|msr|mrs|yield|sev|wfe|wfi|clrex|hvc|smc|eret|sys|dc|ic|tlbi|at|paci|auti|xpac|bti|ldxr|stxr|ldaxr|stlxr|ldxp|stxp|ldaxp|stlxp)")


def xorshift_fill(seed):
    """Region contents from a 32-bit seed; the harness uses the same generator."""
    out = bytearray(MEM_SIZE)
    x = seed or 1
    for i in range(0, MEM_SIZE, 4):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i:i + 4] = struct.pack("<I", x)
    return bytes(out)


def fnv64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h
X_REGS = [globals()[f"UC_ARM64_REG_X{i}"] for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
Q_REGS = [globals()[f"UC_ARM64_REG_Q{i}"] for i in range(32)]
SKIP = re.compile(r"^(b|bl|br|blr|ret|cb|tb|ld|st|prfm|dmb|dsb|isb|svc|brk|hint|nop|adr|cas|swp|msr|mrs|yield|sev|wfe|wfi|clrex|hvc|smc|eret|sys|dc|ic|tlbi|at|paci|auti|xpac|bti)")
FORM_REG = re.compile(r"\b([xwvqdshb])\d+\b")


def form_of(mnemonic, op_str):
    """Mnemonic plus the operand shapes (register classes, arrangements,
    shifts, immediates collapsed), so each encoding variant is sampled."""
    ops = FORM_REG.sub(lambda m: m.group(1), op_str)
    ops = re.sub(r"#-?0x[0-9a-f]+|#-?\d+(\.\d+)?(e[+-]?\d+)?", "#i", ops)
    ops = re.sub(r"\bsp\b|\bwsp\b", "SP", ops)
    return f"{mnemonic} {ops}"


def random_value(rng):
    k = rng.random()
    if k < 0.25:
        return rng.getrandbits(64)
    if k < 0.45:
        return rng.choice([0, 1, 2, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000, 31, 32, 63, 64])
    if k < 0.8:  # floats in both lanes
        f = struct.unpack("<I", struct.pack("<f", rng.choice([0.0, -0.0, 1.0, -1.5, 0.5, 3.25, 1e10, -1e-10, 255.75, 65536.0, rng.uniform(-1000, 1000)])))[0]
        g = struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0]
        return f | (g << 32)
    return struct.unpack("<Q", struct.pack("<d", rng.choice([0.0, 1.0, -2.5, 1e100, rng.uniform(-1e6, 1e6)])))[0]


def run_unicorn(word, state, mem_seed=None, sp=0):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(CODE, 0x1000)
    if mem_seed is not None:
        uc.mem_map(MEM, MEM_SIZE)
        uc.mem_write(MEM, xorshift_fill(mem_seed))
    uc.mem_write(CODE, struct.pack("<I", word))
    # FP/SIMD access on: CPACR_EL1.FPEN = 0b11
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    xs, qs, nzcv = state
    for i, r in enumerate(X_REGS):
        uc.reg_write(r, xs[i])
    for i, r in enumerate(Q_REGS):
        uc.reg_write(r, qs[i])
    uc.reg_write(UC_ARM64_REG_NZCV, nzcv << 28)
    uc.reg_write(UC_ARM64_REG_SP, sp)
    uc.emu_start(CODE, CODE + 4)
    mem_hash = fnv64(bytes(uc.mem_read(MEM, MEM_SIZE))) if mem_seed is not None else 0
    return ([uc.reg_read(r) for r in X_REGS], [uc.reg_read(r) for r in Q_REGS], (uc.reg_read(UC_ARM64_REG_NZCV) >> 28) & 0xF,
            uc.reg_read(UC_ARM64_REG_SP), mem_hash)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("out_dir")
    ap.add_argument("--per-form", type=int, default=3)
    ap.add_argument("--runs", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--table", help="function table json (sample only inside functions)")
    ap.add_argument("--memory", action="store_true", help="test load/store forms instead of register-only ones")
    args = ap.parse_args()
    rng = random.Random(args.seed)

    macho = lief.parse(args.binary)
    seg = next(s for s in macho.segments if s.name == "__TEXT")
    gva, code = seg.virtual_address, bytes(seg.content)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True

    # pick sample addresses with the light disassembler, then decode those in detail
    lite = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    lite.skipdata = True
    picks = collections.defaultdict(list)
    if args.table:
        import json
        ranges = [(f["addr"], f["addr"] + f["size"]) for f in json.load(open(args.table))["functions"]]
    else:
        ranges = [(gva, gva + len(code))]
    for start, end in ranges:
        for address, size, mnemonic, op_str in lite.disasm_lite(code[start - gva:end - gva], start):
            if size != 4 or mnemonic == ".byte" or "pc" in op_str:
                continue
            if args.memory:
                if MEM_SKIP.match(mnemonic) or "[" not in op_str:
                    continue
            elif SKIP.match(mnemonic) or "sp" in op_str:
                continue
            f = form_of(mnemonic, op_str)
            if len(picks[f]) < args.per_form:
                picks[f].append(address)
    forms = {f: [next(md.disasm(code[a - gva:a - gva + 4], a)) for a in addrs] for f, addrs in picks.items()}
    print(f"{len(forms)} instruction forms", file=sys.stderr)

    em = arm64_recomp.Emitter(md, set(), None)
    em.signatures, em.types, em.native_resolvers = {}, {}, set()
    em.sig_by_name, em.icall_cache, em.icall_resolver = {}, {}, None
    func = {"start": 0, "end": 0, "name": "t", "native_calls": {}}

    tests, bodies, skipped = [], [], collections.Counter()
    for f, samples in sorted(forms.items()):
        for ins in samples:
            try:
                lines = em.translate(ins, dict(func, start=ins.address, end=ins.address + 4))
            except Exception as e:
                skipped[type(e).__name__] += 1
                continue
            if any("CallImport" in l or "goto" in l or "Unimplemented" in l for l in lines):
                skipped["control"] += 1
                continue
            if not args.memory and any("Load<" in l or "Store<" in l for l in lines):
                skipped["memory"] += 1
                continue
            mem_regs = set()
            index_regs = set()
            if args.memory:
                for o in ins.operands:
                    if o.type == capstone.arm64.ARM64_OP_MEM:
                        if o.mem.base:
                            mem_regs.add(md.reg_name(o.mem.base))
                        if o.mem.index:
                            index_regs.add(md.reg_name(o.mem.index))
            word = struct.unpack("<I", ins.bytes)[0]
            runs = []
            for _ in range(args.runs):
                xs = [random_value(rng) for _ in range(31)]
                sp = 0
                seed = 0
                if args.memory:
                    seed = rng.getrandbits(32) | 1
                    num = lambda name: {"fp": 29, "lr": 30}.get(name, None) or int(name[1:])
                    for name in mem_regs:
                        val = MEM + 0x8000 + rng.randrange(-64, 64) * 16
                        if name in ("sp", "wsp"):
                            sp = val
                        else:
                            xs[num(name)] = val
                    for name in index_regs:
                        xs[num(name)] = rng.randrange(0, 64)
                    if not sp:
                        sp = MEM + 0x8000
                state = (xs, [random_value(rng) | (random_value(rng) << 64) for _ in range(32)], rng.getrandbits(4))
                try:
                    out = run_unicorn(word, state, seed if args.memory else None, sp)
                except UcError:
                    continue
                runs.append((state, sp, seed, out))
            if not runs:
                skipped["unicorn"] += 1
                continue
            idx = len(tests)
            bodies.append(f"static void t_{idx}(Ctx& c)\n{{\n    " + "\n    ".join(lines) + "\n}\n")
            tests.append((idx, f"{ins.address:#x} {ins.mnemonic} {ins.op_str}", runs))
    print(f"{len(tests)} instructions, skipped {dict(skipped)}", file=sys.stderr)

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "vectors.bin"), "wb") as v:
        for idx, text, runs in tests:
            for (xs, qs, nz), sp, seed, (oxs, oqs, onz, osp, ohash) in runs:
                v.write(struct.pack("<IIQ", idx, seed, sp))
                for st_x, st_q, st_n in ((xs, qs, nz), (oxs, oqs, onz)):
                    v.write(struct.pack("<31Q", *st_x))
                    for q in st_q:
                        v.write(struct.pack("<QQ", q & ((1 << 64) - 1), q >> 64))
                    v.write(struct.pack("<I", st_n))
                v.write(struct.pack("<QQ", osp, ohash))
    with open(os.path.join(args.out_dir, "harness.cpp"), "w") as h:
        h.write('#include <cstdio>\n#include <cstdlib>\n#include <sys/mman.h>\n#include "recomp_ctx.h"\n#include "recomp_simd.h"\n')
        h.write("namespace recomp { uint64_t g_imageBase, g_imageSize; void CallImport(Ctx&, uint64_t) { abort(); }\n"
                "void CallIndirect(Ctx&, uint64_t) { abort(); } void Trap(Ctx&, uint64_t, uint32_t) { abort(); }\n"
                "void Unimplemented(Ctx&, uint64_t, const char*) { abort(); } }\nusing namespace recomp;\n")
        h.write(f"static const uint64_t kMem = {MEM:#x}ull, kMemSize = {MEM_SIZE:#x}ull;\n")
        h.write("".join(bodies))
        h.write("using Test = void (*)(Ctx&);\nstatic const Test kTests[] = {" + ", ".join(f"t_{i}" for i, _, _ in tests) + "};\n")
        h.write("static const char* kNames[] = {" + ", ".join('"' + t.replace('"', "'") + '"' for _, t, _ in tests) + "};\n")
        h.write(HARNESS_MAIN)
    print(f"wrote {args.out_dir}", file=sys.stderr)


HARNESS_MAIN = r'''
struct State { uint64_t x[31]; uint64_t q[32][2]; uint32_t nzcv; };

static bool IsNan(uint64_t lo, bool wide) {
    if (wide) { uint64_t e = (lo >> 52) & 0x7FF, m = lo & ((1ull << 52) - 1); return e == 0x7FF && m; }
    uint32_t v = uint32_t(lo); return ((v >> 23) & 0xFF) == 0xFF && (v & 0x7FFFFF);
}

static void Fill(uint8_t* mem, uint32_t seed) {
    uint32_t x = seed ? seed : 1;
    for (uint64_t i = 0; i < kMemSize; i += 4) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; memcpy(mem + i, &x, 4); }
}

static uint64_t Fnv(const uint8_t* p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static bool ReadState(FILE* f, State& s) {
    return fread(s.x, 8, 31, f) == 31 && fread(s.q, 16, 32, f) == 32 && fread(&s.nzcv, 4, 1, f) == 1;
}

int main(int argc, char** argv) {
    FILE* f = fopen(argc > 1 ? argv[1] : "vectors.bin", "rb");
    if (!f) { perror("vectors"); return 1; }
    uint8_t* mem = (uint8_t*)mmap((void*)kMem, kMemSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mem != (uint8_t*)kMem) { perror("mmap"); return 1; }
    static int reported[sizeof(kTests) / sizeof(kTests[0])];
    int failures = 0, total = 0;
    struct { uint32_t idx, seed; uint64_t sp; } hdr;
    while (fread(&hdr, 16, 1, f) == 1) {
        State in, want;
        uint64_t wantSp, wantHash;
        if (!ReadState(f, in) || !ReadState(f, want) || fread(&wantSp, 8, 1, f) != 1 || fread(&wantHash, 8, 1, f) != 1) break;
        if (hdr.seed) Fill(mem, hdr.seed);
        Ctx c{};
        for (int i = 0; i < 31; i++) c.x[i] = in.x[i];
        for (int i = 0; i < 32; i++) { c.v[i].u64[0] = in.q[i][0]; c.v[i].u64[1] = in.q[i][1]; }
        c.n = (in.nzcv >> 3) & 1; c.z = (in.nzcv >> 2) & 1; c.c = (in.nzcv >> 1) & 1; c.vf = in.nzcv & 1;
        c.sp = hdr.sp;
        kTests[hdr.idx](c);
        total++;
        char diff[256] = "";
        for (int i = 0; i < 31 && !diff[0]; i++)
            if (c.x[i] != want.x[i]) snprintf(diff, sizeof diff, "x%d got %016llx want %016llx", i, (unsigned long long)c.x[i], (unsigned long long)want.x[i]);
        for (int i = 0; i < 32 && !diff[0]; i++)
            for (int h = 0; h < 2 && !diff[0]; h++)
                if (c.v[i].u64[h] != want.q[i][h] && !(IsNan(c.v[i].u64[h], true) && IsNan(want.q[i][h], true))
                    && !(IsNan(c.v[i].u64[h], false) && IsNan(want.q[i][h], false)))
                    snprintf(diff, sizeof diff, "v%d.d[%d] got %016llx want %016llx", i, h, (unsigned long long)c.v[i].u64[h], (unsigned long long)want.q[i][h]);
        uint32_t nz = (c.n << 3) | (c.z << 2) | (c.c << 1) | c.vf;
        if (!diff[0] && nz != want.nzcv) snprintf(diff, sizeof diff, "nzcv got %x want %x", nz, want.nzcv);
        if (!diff[0] && hdr.seed && c.sp != wantSp) snprintf(diff, sizeof diff, "sp got %llx want %llx", (unsigned long long)c.sp, (unsigned long long)wantSp);
        if (!diff[0] && hdr.seed && Fnv(mem, kMemSize) != wantHash) snprintf(diff, sizeof diff, "memory differs");
        if (diff[0]) {
            failures++;
            if (reported[hdr.idx]++ < 1) printf("FAIL %s: %s\n", kNames[hdr.idx], diff);
        }
    }
    printf("%d runs, %d mismatches\n", total, failures);
    return failures != 0;
}
'''

if __name__ == "__main__":
    main()
