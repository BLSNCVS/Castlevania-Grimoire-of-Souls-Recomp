# Creates case-alias symlinks for headers that sources include with a
# different letter case than the files have (Windows ignores case, Linux
# does not). Reads "file not found" errors from compiler logs.
# usage: python3 -I fix_case.py <alias dir> <log files...> -- <include dirs...>
import sys, os, re
alias_dir = sys.argv[1]
sep = sys.argv.index("--")
logs, dirs = sys.argv[2:sep], sys.argv[sep + 1:]
missing = set()
for log in logs:
    for m in re.finditer(r"fatal error: '([^']+)' file not found", open(log, errors="replace").read()):
        missing.add(m.group(1))
index = {}
for d in dirs:
    for root, _, files in os.walk(d):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), d)
            index.setdefault(rel.lower(), os.path.join(root, f))
            index.setdefault(f.lower(), os.path.join(root, f))
os.makedirs(alias_dir, exist_ok=True)
made = 0
for name in sorted(missing):
    target = index.get(name.lower()) or index.get(os.path.basename(name).lower())
    if not target:
        print("no match for", name)
        continue
    link = os.path.join(alias_dir, name)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    if not os.path.lexists(link):
        os.symlink(target, link)
        made += 1
print("aliases created:", made)
