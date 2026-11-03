# Surveys asset bundles: build target, object types, texture formats, shaders.
# usage: python3 -I survey_assets.py <Data/Raw/ab dir> [extra serialized files...]
import sys,os,collections,json,UnityPy
root=sys.argv[1]
types=collections.Counter(); fmts=collections.Counter(); plat=collections.Counter()
shaders={}; errs=0
files=[os.path.join(root,f) for f in sorted(os.listdir(root)) if f.endswith(('.ab','.sa'))]
extra=sys.argv[2:]
for i,f in enumerate(files+extra):
    try:
        env=UnityPy.load(f)
        for name,sf in env.files.items():
            for sub in getattr(sf,'files',{}).values():
                tp=getattr(sub,'target_platform',None)
                if tp is not None: plat[str(tp)]+=1
        for o in env.objects:
            t=o.type.name; types[t]+=1
            if t=="Texture2D":
                d=o.read(); fmts[str(d.m_TextureFormat)]+=1
            elif t=="Shader":
                d=o.read()
                nm=getattr(getattr(d,'m_ParsedForm',None),'m_Name',None) or getattr(d,'m_Name','?')
                pl=[str(p) for p in getattr(d,'platforms',[])]
                shaders[nm]=pl
    except Exception as e:
        errs+=1
print("platforms",plat.most_common())
print("types",types.most_common(40))
print("texfmts",fmts.most_common())
print("shaders",len(shaders)); 
for k,v in sorted(shaders.items()): print("  ",k,v)
print("errors",errs)
