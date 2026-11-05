import re
import sys
from pathlib import Path

PROPS = (('ID_Start', 'UNICODE_ID_START'), ('ID_Continue', 'UNICODE_ID_CONTINUE'))
LINE = re.compile(r'^([0-9A-F]+)(?:\.\.([0-9A-F]+))?\s*;\s*(\w+)')


def ranges(text, prop):
  found = []
  for line in text.splitlines():
    m = LINE.match(line)
    if m and m.group(3) == prop: found.append((int(m.group(1), 16), int(m.group(2) or m.group(1), 16)))
  out = []
  for lo, hi in sorted(found):
    if out and out[-1][1] + 1 >= lo: out[-1][1] = max(out[-1][1], hi)
    else: out.append([lo, hi])
  return out


def section(text, prop, macro):
  body = ''.join(f'{macro}(0x{lo:X}, 0x{hi:X})\n' for lo, hi in ranges(text, prop))
  return f'#ifdef {macro}\n{body}#undef {macro}\n#endif\n'


def main(src, dst):
  text = Path(src).read_text(encoding='utf-8')
  Path(dst).write_text('\n'.join(section(text, prop, macro) for prop, macro in PROPS))


if __name__ == '__main__':
  main(sys.argv[1], sys.argv[2])
