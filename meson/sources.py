import sys, os, glob, json

data = json.load(open(sys.argv[1]))
group_name = sys.argv[2]
section = None
if ':' in group_name:
  group_name, section = group_name.split(':', 1)

group = data[group_name]
if section:
  group = group['sections'][section]
base = sys.argv[3] if len(sys.argv) > 3 else ''

matched = set()
for f in group.get('files', []):
  matched.add(os.path.join(base, f) if base else f)

for p in group.get('patterns', []):
  full = os.path.join(base, p) if base else p
  matched.update(glob.glob(full, recursive=True))

excluded = set(os.path.join(base, e) if base else e for e in group.get('exclude', []))
matched -= excluded
print('\n'.join(sorted(matched)))
