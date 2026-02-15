import sys, glob, json

data = json.load(open(sys.argv[1]))
group = data[sys.argv[2]]

matched = set()
for p in group['patterns']:
  matched.update(glob.glob(p, recursive=True))

matched -= set(group.get('exclude', []))
print('\n'.join(sorted(matched)))