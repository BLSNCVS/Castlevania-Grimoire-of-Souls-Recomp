# Adds a Direct3D 11 variant to every ComputeShader in Unity asset files,
# translated from the Metal variant (msl_compute2hlsl.py) and compiled with
# d3dcompiler_47 (hlslc.exe, as convert_shaders.py does).
#
# usage: python3 convert_compute.py <hlslc dir> <work dir> <out dir> <asset files...>
#
# Unity picks a ComputeShader variant by GfxDeviceRenderer (Metal 16,
# D3D11 2) and hands D3D11 kernels' code to CreateComputeShader as is, so
# the variant holds plain DXBC plus the register of every resource.
import copy, os, subprocess, sys

import UnityPy

import msl_compute2hlsl

METAL, D3D11 = 16, 2
METAL_HEADER = 20  # HLSLcc's Metal blob header before the source


def compile_all(sources, hlslc_dir, work):
    os.makedirs(work, exist_ok=True)
    native = os.name == "nt"
    to_path = (lambda p: p) if native else (lambda p: "Z:" + p)
    lines = []
    for n, src in enumerate(sources):
        path = os.path.join(work, f"k{n:04d}.hlsl")
        with open(path, "w") as f:
            f.write(src)
        lines.append(f"cs_5_0 {to_path(path)} {to_path(path[:-5] + '.dxbc')}")
    lst = os.path.join(work, "list.txt")
    with open(lst, "w") as f:
        f.write("\n".join(lines) + "\n")
    exe = os.path.join(hlslc_dir, "hlslc.exe")
    cmd = [exe, to_path(lst)] if native else ["wine", exe, to_path(lst)]
    env = dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="d3dcompiler_47=n")
    print(subprocess.run(cmd, env=env, capture_output=True, text=True).stdout.strip())
    out = []
    for n in range(len(sources)):
        path = os.path.join(work, f"k{n:04d}.dxbc")
        out.append(open(path, "rb").read() if os.path.exists(path) else None)
    return out


def d3d11_resources(inner, bindings, inline_regs):
    """The variant's resource tables with D3D11 registers, keyed by name."""
    by_name = {b.name: b for b in bindings}
    reg = lambda name, default=-1: by_name[name].register if name in by_name else default
    res = copy.deepcopy({k: inner[k] for k in ("cbs", "textures", "builtinSamplers", "inBuffers", "outBuffers")})
    for e in res["cbs"]:
        e["bindPoint"] = reg(e["name"])
    for e in res["textures"]:
        e["bindPoint"] = reg(e["name"])
        e["samplerBindPoint"] = reg("sampler" + e["name"])
    for key in ("inBuffers", "outBuffers"):
        for e in res[key]:
            e["bindPoint"] = reg(e["name"])
    for kind, key in (("buf", "inBuffers"), ("rwbuf", "outBuffers"), ("rwtex", "outBuffers")):
        listed = {e["name"] for e in res[key]}
        for b in bindings:
            if b.kind == kind and b.name not in listed:
                raise msl_compute2hlsl.Unsupported(f"{b.name} missing from {key}")
    # the only inline sampler these kernels use (linear/clamp) is what an
    # unbound D3D11 sampler slot gives: nothing to bind
    if inline_regs and res["builtinSamplers"]:
        raise msl_compute2hlsl.Unsupported("builtin samplers")
    return res


def convert_env(env, hlslc_dir, work, log):
    jobs = []  # (variant dict, kernel index, map index, inner, bindings, inline)
    sources = []
    shaders = []
    for obj in env.objects:
        if obj.type.name != "ComputeShader":
            continue
        tree = obj.read_typetree()
        variants = tree["variants"]
        metal = [v for v in variants if v["targetRenderer"] == METAL]
        if not metal or any(v["targetRenderer"] == D3D11 for v in variants):
            continue
        new = copy.deepcopy(metal[0])
        new["targetRenderer"] = D3D11
        ok = True
        for ki, kernel in enumerate(new["kernels"]):
            for mi, (keywords, inner) in enumerate(kernel["variantMap"]):
                msl = bytes(inner["code"])[METAL_HEADER:].decode("utf-8", "replace")
                try:
                    hlsl, bindings, inline = msl_compute2hlsl.translate(msl, inner["threadGroupSize"])
                    res = d3d11_resources(inner, bindings, inline)
                except msl_compute2hlsl.Unsupported as e:
                    print(f"  {tree['m_Name']}.{kernel['name']}: {e}")
                    ok = False
                    continue
                inner.update(res)
                jobs.append((inner, len(sources)))
                sources.append(hlsl)
        shaders.append((obj, tree, new, ok))
    if not sources:
        return
    codes = compile_all(sources, hlslc_dir, work)
    for inner, n in jobs:
        inner["code"] = codes[n]
    for obj, tree, new, ok in shaders:
        kernels = [inner for k in new["kernels"] for _, inner in k["variantMap"]]
        if not ok or any(not isinstance(inner["code"], (bytes, bytearray)) for inner in kernels):
            log["compute shaders left Metal-only"] += 1
            print(f"  {tree['m_Name']}: left Metal-only")
            continue
        for inner in kernels:
            inner["code"] = list(inner["code"])
        tree["variants"].append(new)
        obj.save_typetree(tree)
        log["compute shaders converted"] += 1
        print(f"  {tree['m_Name']}: {len(kernels)} kernels")


def main():
    import collections
    hlslc_dir, work, out_dir = sys.argv[1:4]
    log = collections.Counter()
    for n, path in enumerate(sys.argv[4:]):
        env = UnityPy.load(path)
        print(path)
        convert_env(env, hlslc_dir, os.path.join(work, f"f{n}"), log)
        os.makedirs(out_dir, exist_ok=True)
        is_bundle = env.file.__class__.__name__ == "BundleFile"
        with open(os.path.join(out_dir, os.path.basename(path)), "wb") as f:
            f.write(env.file.save(packer="lz4") if is_bundle else env.file.save())
    for k, v in log.most_common():
        print(f"{k}: {v}")


if __name__ == "__main__":
    main()
