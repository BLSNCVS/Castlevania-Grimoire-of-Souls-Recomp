# Declares D3D11 as the graphics API the data was built for (the macOS build
# lists only Metal, and the Windows player refuses to start D3D11 otherwise).
#
# usage: python3 -I patch_build_settings.py <globalgamemanagers> <out>
import sys
import UnityPy

GFX_D3D11 = 2

env = UnityPy.load(sys.argv[1])
for obj in env.objects:
    if obj.type.name == "BuildSettings":
        tree = obj.read_typetree()
        print("m_GraphicsAPIs", tree["m_GraphicsAPIs"], "->", [GFX_D3D11])
        tree["m_GraphicsAPIs"] = [GFX_D3D11]
        obj.save_typetree(tree)
with open(sys.argv[2], "wb") as f:
    f.write(env.file.save())
