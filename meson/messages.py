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
  messages = tomllib.load(f)["messages"]

print("#ifndef MESSAGES_H")
print("#define MESSAGES_H")
print("")
print("typedef struct {")
for name in messages:
  print(f"  const char *{name};")
print("} ant_messages_t;")
print("")
print("static const ant_messages_t msg = {")
for name, value in messages.items():
  escaped = value.encode("unicode_escape").decode().replace('"', '\\"')
  print(f'  .{name} = "{escaped}",')
print("};")
print("")
print("#endif")
