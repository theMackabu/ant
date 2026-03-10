import sys

try:
  import tomllib
except ModuleNotFoundError:
  try:
    import tomli as tomllib
  except ModuleNotFoundError as exc:
    raise ModuleNotFoundError(
      "No TOML parser available. Use Python 3.11+ or install 'tomli'."
    ) from exc

with open(sys.argv[1], "rb") as f:
  theme = tomllib.load(f)

colors = theme.get("colors", {})

def to_enum(name):
  return "HL_" + name.upper()

print("#ifndef THEME_H")
print("#define THEME_H")
print("")
print('#include "highlight.h"')
print("")
print("static const char *hl_theme_color(hl_token_class cls) {")
print("  switch (cls) {")
for name, value in colors.items():
  escaped = value.replace("\\", "\\\\").replace('"', '\\"')
  print(f'    case {to_enum(name)}: return "{escaped}";')
print("    default: return NULL;")
print("  }")
print("}")
print("")
print("#endif")
