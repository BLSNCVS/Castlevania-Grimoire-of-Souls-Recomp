# Function table for a plain arm64 Mach-O library (the SPFX plugin): every
# LC_FUNCTION_STARTS entry in __text, named from the symbol table, plus the
# stubs that calls go through, named after the symbol each one binds.
#
# usage: python3 -I function_table.py <arm64 Mach-O> <out.json>
import json, struct, sys

import capstone
import lief

path, out = sys.argv[1:3]
b = lief.parse(path)
text = b.get_section("__text")
tva, tend = text.virtual_address, text.virtual_address + text.size

names = {}
for s in b.symbols:
    if s.value and tva <= s.value < tend and s.name:
        names.setdefault(s.value, s.name)
starts = sorted({a for a in b.function_starts.functions if tva <= a < tend} | set(names))
functions = []
for i, a in enumerate(starts):
    end = starts[i + 1] if i + 1 < len(starts) else tend
    functions.append({"addr": a, "size": end - a, "name": names.get(a, f"sub_{a:x}")})

# stub: [adrp x16, page | nop]; ldr x16, [x16, #off] | ldr x16, literal; br x16
bound = {}
for bi in b.bindings:
    if bi.has_symbol:
        bound[bi.address] = bi.symbol.name
stubs = b.get_section("__stubs")
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True
imports = []
data = bytes(stubs.content)
for off in range(0, len(data), 12):
    ins = list(md.disasm(data[off:off + 12], stubs.virtual_address + off))
    if len(ins) != 3 or ins[1].mnemonic != "ldr" or ins[2].mnemonic != "br":
        continue
    if ins[0].mnemonic == "adrp":
        slot = ins[0].operands[1].imm + ins[1].operands[1].mem.disp
    else:
        slot = ins[1].operands[1].imm
    imports.append({"addr": stubs.virtual_address + off, "symbol": bound.get(slot, ""), "slot": slot})
# pointers bound in data (GOT and others) that code may load and call
data_bindings = [{"addr": a, "symbol": n} for a, n in sorted(bound.items())]

json.dump({"functions": functions, "imports": imports, "data_bindings": data_bindings}, open(out, "w"), indent=0)
print(f"{len(functions)} functions, {len(imports)} stubs, {sum(1 for i in imports if not i['symbol'])} unnamed")
