# Differential tests for the recompiler: runs game functions with primitive
# signatures in Unicorn (the arm64 reference) and records inputs/outputs as
# C++ test vectors for the recompiled code.
#
# usage: python3 -I difftest.py <GameAssembly arm64> <function_table.json> <script.json>
#            <out_dir> [--max N] [--runs N]
#
# Writes <out_dir>/functions.txt (addresses to recompile) and
# <out_dir>/difftest_vectors.inc (consumed by recomp/tests/difftest.cpp).
import sys, json, argparse, random, struct, re, math
import lief
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_INVALID, UcError
from unicorn.arm64_const import *

BASE = 0x200000000
STACK_TOP = 0x7f0000000
STACK_SIZE = 0x100000
STOP = 0x7e0000000  # return address sentinel
Q_REGS = [globals()[f"UC_ARM64_REG_Q{i}"] for i in range(8)]

INT_TYPES = {"int32_t": 32, "uint32_t": 32, "int64_t": 64, "uint64_t": 64, "bool": 8, "uint8_t": 8,
             "int8_t": 8, "int16_t": 16, "uint16_t": 16, "Il2CppChar": 16, "intptr_t": 64, "uintptr_t": 64}
FLOAT_TYPES = {"float": 32, "double": 64}


def parse_signature(sig):
    """'float Foo__Bar (float a, int32_t b, const MethodInfo* method);' -> (ret, [types]) or None"""
    m = re.match(r"\s*(.+?)\s+\w+\s*\((.*)\);\s*$", sig)
    if not m:
        return None
    ret = m.group(1).strip()
    params = [p.strip() for p in m.group(2).split(",") if p.strip()]
    if not params or not params[-1].startswith("const MethodInfo*"):
        return None
    types = []
    for p in params[:-1]:
        t = p.rsplit(" ", 1)[0].strip()
        if t not in INT_TYPES and t not in FLOAT_TYPES:
            return None
        types.append(t)
    if ret != "void" and ret not in INT_TYPES and ret not in FLOAT_TYPES:
        return None
    if ret == "void":
        return None
    return ret, types


def rebased_image(path):
    b = lief.parse(path)
    segs = [s for s in b.segments if s.name != "__PAGEZERO"]
    size = max(s.virtual_address + s.virtual_size for s in segs)
    size = (size + 0xFFFF) & ~0xFFFF
    mem = bytearray(size)
    data = open(path, "rb").read()
    for s in segs:
        mem[s.virtual_address:s.virtual_address + s.file_size] = data[s.file_offset:s.file_offset + s.file_size]
    # classic dyld rebase opcodes
    info = b.dyld_info
    ops = bytes(info.rebase_opcodes)
    seglist = list(b.segments)
    p, addr, seg = 0, 0, 0

    def uleb():
        nonlocal p
        r, sh = 0, 0
        while True:
            byte = ops[p]; p += 1
            r |= (byte & 0x7f) << sh; sh += 7
            if not byte & 0x80:
                return r

    def rebase(a):
        va = seglist[seg].virtual_address + a
        v = struct.unpack_from("<Q", mem, va)[0]
        struct.pack_into("<Q", mem, va, (v + BASE) & 0xFFFFFFFFFFFFFFFF)

    while p < len(ops):
        byte = ops[p]; p += 1
        op, imm = byte & 0xF0, byte & 0x0F
        if op == 0x00:
            break
        elif op == 0x20:
            seg = imm; addr = uleb()
        elif op == 0x30:
            addr += uleb()
        elif op == 0x40:
            addr += imm * 8
        elif op == 0x50:
            for _ in range(imm):
                rebase(addr); addr += 8
        elif op == 0x60:
            for _ in range(uleb()):
                rebase(addr); addr += 8
        elif op == 0x70:
            rebase(addr); addr += uleb() + 8
        elif op == 0x80:
            n, skip = uleb(), uleb()
            for _ in range(n):
                rebase(addr); addr += skip + 8
    gen = b.get_section("il2cpp")
    return bytes(mem), (gen.virtual_address, gen.virtual_address + gen.size)


def random_value(t, rng):
    if t in FLOAT_TYPES:
        choice = rng.random()
        if choice < 0.1:
            v = rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-7, 1e7])
        elif choice < 0.6:
            v = rng.uniform(-10, 10)
        else:
            v = rng.uniform(-1e4, 1e4)
        if t == "float":
            return struct.unpack("<I", struct.pack("<f", v))[0]
        return struct.unpack("<Q", struct.pack("<d", v))[0]
    bits = INT_TYPES[t]
    if t == "bool":
        return rng.randint(0, 1)
    choice = rng.random()
    if choice < 0.2:
        v = rng.choice([0, 1, 2, 3, 7, 8, 16, 100, 255, 256, -1, -2, -100])
    elif choice < 0.7:
        v = rng.randint(-1000, 1000)
    else:
        v = rng.getrandbits(bits)
    return v & ((1 << bits) - 1)


def sign_extend(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Runner:
    def __init__(self, image, gen_range):
        self.image = image
        self.gen_lo, self.gen_hi = gen_range
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(BASE, len(image))
        self.uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE)
        self.uc.mem_map(STOP, 0x1000)
        self.uc.mem_write(BASE, image)
        self.uc.hook_add(UC_HOOK_CODE, self.on_code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)
        self.executed = set()
        self.bad = None

    def on_code(self, uc, address, size, user):
        if address == STOP:
            uc.emu_stop()
            return
        off = address - BASE
        if not (self.gen_lo <= off < self.gen_hi):
            self.bad = f"leaves game code at {off:#x}"
            uc.emu_stop()
            return
        self.executed.add(off)

    def on_write(self, uc, access, address, size, value, user):
        if not (STACK_TOP - STACK_SIZE <= address < STACK_TOP):
            self.bad = f"writes global memory at {address:#x}"
            uc.emu_stop()

    def run(self, func, types, args):
        uc = self.uc
        self.bad = None
        xi, vi = 0, 0
        for t, a in zip(types, args):
            if t in FLOAT_TYPES:
                uc.reg_write(Q_REGS[vi], a)
                vi += 1
            else:
                uc.reg_write(UC_ARM64_REG_X0 + xi, a)
                xi += 1
        uc.reg_write(UC_ARM64_REG_X0 + xi, 0)  # const MethodInfo* = null
        uc.reg_write(UC_ARM64_REG_SP, STACK_TOP - 0x100)
        uc.reg_write(UC_ARM64_REG_LR, STOP)
        uc.mem_write(STACK_TOP - STACK_SIZE, bytes(STACK_SIZE))
        try:
            uc.emu_start(BASE + func, STOP, timeout=2_000_000, count=200000)
        except UcError as e:
            self.bad = f"fault {e}"
        if self.bad:
            return None
        if uc.reg_read(UC_ARM64_REG_PC) != STOP:
            return None
        x0 = uc.reg_read(UC_ARM64_REG_X0)
        q0 = uc.reg_read(UC_ARM64_REG_Q0)
        return x0, q0 & 0xFFFFFFFFFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("table")
    ap.add_argument("script")
    ap.add_argument("out_dir")
    ap.add_argument("--max", type=int, default=2000)
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    image, gen_range = rebased_image(args.binary)
    funcs = {f["addr"] for f in json.load(open(args.table))["functions"]}

    candidates = []
    seen = set()
    for m in json.load(open(args.script))["ScriptMethod"]:
        a = m["Address"]
        if a in seen or a not in funcs:
            continue
        sig = parse_signature(m["Signature"])
        if sig and "__this" not in m["Signature"] and \
                sum(t in INT_TYPES for t in sig[1]) <= 7 and sum(t in FLOAT_TYPES for t in sig[1]) <= 8:
            seen.add(a)
            candidates.append((a, m["Name"], sig))
    rng.shuffle(candidates)
    print(f"candidate functions with primitive signatures: {len(candidates)}")

    runner = Runner(image, gen_range)
    vectors, executed, accepted = [], set(), 0
    reasons = {}
    for a, name, (ret, types) in candidates:
        if accepted >= args.max:
            break
        runner.executed = set()
        results = []
        ok = True
        for _ in range(args.runs):
            inputs = [random_value(t, rng) for t in types]
            out = runner.run(a, types, inputs)
            if out is None:
                ok = False
                key = (runner.bad or "did not return").split(" at ")[0]
                reasons[key] = reasons.get(key, 0) + 1
                break
            results.append((inputs, out))
        if not ok:
            continue
        accepted += 1
        executed |= runner.executed
        for inputs, (x0, q0) in results:
            vectors.append((a, name, ret, types, inputs, x0, q0))

    # functions to recompile: every function entry touched while executing
    starts = sorted(funcs)
    import bisect
    touched = set()
    for pc in executed:
        i = bisect.bisect_right(starts, pc) - 1
        touched.add(starts[i])

    import os
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "functions.txt"), "w") as f:
        f.write("\n".join(hex(t) for t in sorted(touched)) + "\n")
    with open(os.path.join(args.out_dir, "difftest_vectors.inc"), "w") as f:
        f.write("// Generated by tools/recomp/difftest.py\n")
        for a, name, ret, types, inputs, x0, q0 in vectors:
            kinds = "".join("f" if t in FLOAT_TYPES else "i" for t in types)
            ret_kind = {"float": "kF32", "double": "kF64"}.get(ret, {8: "kI8", 16: "kI16", 32: "kI32", 64: "kI64"}[INT_TYPES.get(ret, 64)])
            ins = ", ".join(f"{v:#x}ull" for v in inputs) or "0"
            expected = q0 if ret in FLOAT_TYPES else x0
            f.write(f'{{ {a:#x}, "{name[:60]}", "{kinds}", {{ {ins} }}, {ret_kind}, {expected:#x}ull }},\n')
    print(f"accepted functions: {accepted}, vectors: {len(vectors)}, functions to recompile: {len(touched)}")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:10]:
        print(f"  rejected {v:6d}: {k}")


if __name__ == "__main__":
    main()
