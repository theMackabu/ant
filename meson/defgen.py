import re
import sys

DEF = re.compile(r"^(\s*)#(\s*)def\s+(.*?)\s*$")
ENDDEF = re.compile(r"^\s*#\s*enddef\s*$")
HEAD = re.compile(r"^[A-Za-z_]\w*(\([^()]*\))?$")
LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def fail(path, lineno, message):
  sys.stderr.write(f"{path}:{lineno}: error: {message}\n")
  raise SystemExit(1)


def emit(out, head, body):
  lines = [head] + [line.rstrip() for line in body]
  width = max(len(line.expandtabs()) for line in lines[:-1]) if len(lines) > 1 else 0
  for line in lines[:-1]:
    out.append(line + " " * (width - len(line.expandtabs())) + " \\")
  out.append(lines[-1])
  out.append("")


def expand(path, text):
  out = [f'#line 1 "{path}"']
  head, body, start = None, [], 0
  lines = text.split("\n")
  if lines[-1] == "":
    lines.pop()
  for lineno, line in enumerate(lines, 1):
    opening = DEF.match(line)
    if head is None:
      if ENDDEF.match(line):
        fail(path, lineno, "#enddef without #def")
      if not opening:
        out.append(line)
        continue
      if not HEAD.match(opening.group(3)):
        fail(path, lineno, "#def takes a macro name and optional parameter list, with the body on the following lines")
      head, body, start = f"{opening.group(1)}#{opening.group(2)}define {opening.group(3)}", [], lineno
      continue
    if ENDDEF.match(line):
      emit(out, head, body)
      head = None
      continue
    if opening:
      fail(path, lineno, f"#def inside the #def opened on line {start}")
    if line.lstrip().startswith("#"):
      fail(path, lineno, "preprocessing directives are not allowed inside #def")
    if line.rstrip().endswith("\\"):
      fail(path, lineno, "line continuations are not needed inside #def")
    if "//" in LITERAL.sub("", line):
      fail(path, lineno, "// comments would swallow the rest of the macro; use /* */")
    body.append(line)
  if head is not None:
    fail(path, start, "#def without #enddef")
  return "\n".join(out) + "\n"


def main():
  source, output = sys.argv[1], sys.argv[2]
  with open(source, encoding="utf-8") as f:
    text = f.read()
  result = expand(source, text)
  with open(output, "w", encoding="utf-8") as f:
    f.write(result)
  if len(sys.argv) > 3:
    with open(sys.argv[3], "w", encoding="utf-8") as f:
      f.write(" ".join(p.replace(" ", "\\ ") for p in (output + ":", source, __file__)) + "\n")


main()
