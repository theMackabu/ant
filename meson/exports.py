import subprocess
import sys

PREFIXES = ("_napi_", "_uv_")
EXCLUDE = ("_uv_link_",)

symbols = set()
nm_command = sys.argv[1]
for archive in sys.argv[2:]:
  nm = subprocess.run(
    [nm_command, "-m", archive], capture_output=True, text=True, check=True
  )
  for line in nm.stdout.splitlines():
    if "(undefined)" in line or " private external " in line:
      continue
    if " external " not in line:
      continue
    name = line.split()[-1]
    if name.startswith(PREFIXES) and not name.startswith(EXCLUDE):
      symbols.add(name)

if not symbols:
  raise SystemExit("exports.py: no exportable symbols found")

for name in sorted(symbols):
  print(name)
