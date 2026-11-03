# Lists native plugin externs (P/Invoke) declared in the game assemblies.
# usage: python3 -I externs.py <Il2CppDumper dump.cs>
import re,bisect,sys
lines=open(sys.argv[1],encoding="utf-8",errors="replace").read().split("\n")
imgs=[]
for l in lines:
    m=re.match(r"// Image \d+: (\S+) - (\d+)",l)
    if m: imgs.append((int(m.group(2)),m.group(1)))
    elif imgs: break
imgs.sort(); starts=[s for s,_ in imgs]
cur=cls=None
for l in lines:
    m=re.search(r"(class|struct) (\S+).*// TypeDefIndex: (\d+)",l)
    if m:
        cls=m.group(2); cur=imgs[bisect.bisect_right(starts,int(m.group(3)))-1][1]
    if "static extern" in l and cur in ("Assembly-CSharp.dll","Assembly-CSharp-firstpass.dll"):
        print(cur.split('.')[0][-9:], cls, l.strip().replace("{ }",""))
