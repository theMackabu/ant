import pathlib
import subprocess
import sys


def main():
  if len(sys.argv) != 4:
    print('usage: npm_ci.py <npm> <tools-dir> <stamp>', file=sys.stderr)
    return 2

  npm, tools_dir, stamp = sys.argv[1:]
  subprocess.run([npm, 'ci'], cwd=tools_dir, check=True)
  pathlib.Path(stamp).write_text('ok\n')
  return 0


if __name__ == '__main__':
  raise SystemExit(main())
