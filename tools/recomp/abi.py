# Shared ABI description for the bridge generators: parses Il2CppDumper method
# signatures and emits C++ that moves arguments between the arm64 Ctx
# (AAPCS64, Apple variant) and native Windows x64 calls.
import re

INTS = {"int32_t": ("int32_t", True, 4), "uint32_t": ("uint32_t", False, 4), "int64_t": ("int64_t", True, 8),
        "uint64_t": ("uint64_t", False, 8), "bool": ("bool", False, 1), "uint8_t": ("uint8_t", False, 1),
        "int8_t": ("int8_t", True, 1), "int16_t": ("int16_t", True, 2), "uint16_t": ("uint16_t", False, 2),
        "Il2CppChar": ("uint16_t", False, 2), "intptr_t": ("int64_t", True, 8), "uintptr_t": ("uint64_t", False, 8)}
FLOATS = {"float": 4, "double": 8}


def split_signature(sig):
    m = re.match(r"\s*(.+?)\s+\w+\s*\((.*)\);\s*$", sig)
    ret = m.group(1).strip()
    params = [p.strip().rsplit(" ", 1)[0].strip() for p in m.group(2).split(",") if p.strip()]
    return ret, params


def normalize(t, types):
    """Type key: P, an int type, float/double, void, or S<size>_<align>_<class>."""
    if t.endswith("*"):
        return "P"
    if t in INTS or t in FLOATS or t == "void":
        return t
    info = types[t]
    cls = info["arg"]
    if cls["kind"] == "hfa":
        tag = f"hfa{cls['count']}{cls['elem'][0]}"
    elif cls["kind"] == "int":
        tag = f"int{cls['regs']}"
    else:
        tag = "indirect"
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


def struct_parts(key):
    size, align, cls = key[1:].split("_", 2)
    return int(size), int(align), cls


class Unpacker:
    """Reads arguments of a native call from the guest Ctx (arm64 caller)."""

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

    def take(self, key, name):
        t = native_type(key)
        L = self.lines
        if key == "P" or key in INTS:
            if self.ngrn < 8:
                L.append(f"{t} {name} = ({t})(c.x[{self.ngrn}]);")
                self.ngrn += 1
            else:
                size = 8 if key == "P" else INTS[key][2]
                off = self.stack_slot(size, size)
                L.append(f"{t} {name}; memcpy(&{name}, reinterpret_cast<void*>(c.sp + {off}), {size});")
            return
        if key in FLOATS:
            if self.nsrn < 8:
                lane = "f32" if key == "float" else "f64"
                L.append(f"{t} {name} = c.v[{self.nsrn}].{lane}[0];")
                self.nsrn += 1
            else:
                size = FLOATS[key]
                off = self.stack_slot(size, size)
                L.append(f"{t} {name}; memcpy(&{name}, reinterpret_cast<void*>(c.sp + {off}), {size});")
            return
        size, align, cls = struct_parts(key)
        if cls.startswith("hfa"):
            n, elem = int(cls[3]), cls[4]
            esize = 4 if elem == "f" else 8
            lane = "u32" if esize == 4 else "u64"
            if self.nsrn + n <= 8:
                parts = " ".join(f"memcpy({name}.bytes + {k * esize}, &c.v[{self.nsrn + k}].{lane}[0], {esize});" for k in range(n))
                L.append(f"{t} {name}; {parts}")
                self.nsrn += n
            else:
                self.nsrn = 8
                off = self.stack_slot(size, max(align, 8))
                L.append(f"{t} {name}; memcpy({name}.bytes, reinterpret_cast<void*>(c.sp + {off}), {size});")
            return
        if cls.startswith("int"):
            regs = int(cls[3])
            if self.ngrn + regs <= 8:
                L.append(f"{t} {name} = CopyFromRegs<{t}>(&c.x[{self.ngrn}]);")
                self.ngrn += regs
            else:
                self.ngrn = 8
                off = self.stack_slot(size, max(align, 8))
                L.append(f"{t} {name}; memcpy({name}.bytes, reinterpret_cast<void*>(c.sp + {off}), {size});")
            return
        # indirect: the guest passed a pointer to its copy
        ptr = f"{name}_ptr"
        self.take("P", ptr)
        L.append(f"{t} {name}; memcpy({name}.bytes, {ptr}, {size});")


def store_result(key, expr):
    """C++ statements writing a native result into the guest return registers."""
    if key == "void":
        return [f"{expr};"]
    t = native_type(key)
    if key == "P":
        return [f"c.x[0] = reinterpret_cast<uint64_t>({expr});"]
    if key in INTS:
        _, signed, _ = INTS[key]
        return [f"c.x[0] = uint64_t(int64_t({expr}));" if signed else f"c.x[0] = uint64_t({expr});"]
    if key == "float":
        return [f"SetS(c.v[0], {expr});"]
    if key == "double":
        return [f"SetD(c.v[0], {expr});"]
    size, align, cls = struct_parts(key)
    if cls.startswith("hfa"):
        n, elem = int(cls[3]), cls[4]
        esize = 4 if elem == "f" else 8
        setter = "SetS32" if esize == 4 else "SetD64"
        ctype = "uint32_t" if esize == 4 else "uint64_t"
        parts = " ".join(f"{{ {ctype} e; memcpy(&e, r.bytes + {k * esize}, {esize}); {setter}(c.v[{k}], e); }}" for k in range(n))
        return [f"{{ {t} r = {expr}; {parts} }}"]
    if cls.startswith("int"):
        return [f"{{ {t} r = {expr}; CopyToRegs(&c.x[0], r); }}"]
    return [f"{{ {t} r = {expr}; memcpy(reinterpret_cast<void*>(c.x[8]), r.bytes, {size}); }}"]


def native_call(ret, params, target):
    """C++ block calling native function pointer `target` with guest arguments."""
    u = Unpacker()
    names = [f"n{i}" for i in range(len(params))]
    for key, name in zip(params, names):
        u.take(key, name)
    fn_type = f"{native_type(ret)} (*)({', '.join(native_type(k) for k in params)})"
    call = f"reinterpret_cast<{fn_type}>({target})({', '.join(names)})"
    return u.lines + ["SetGuestStackPointer(c.sp);"] + store_result(ret, call)
