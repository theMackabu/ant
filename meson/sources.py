import sys, os, glob, json

data = json.load(open(sys.argv[1]))
group = data[sys.argv[2]]
base = sys.argv[3] if len(sys.argv) > 3 else ''

matched = set()
for p in group['patterns']:
  full = os.path.join(base, p) if base else p
  matched.update(glob.glob(full, recursive=True))

excluded = set(os.path.join(base, e) if base else e for e in group.get('exclude', []))
matched -= excluded
print('\n'.join(sorted(matched)))
