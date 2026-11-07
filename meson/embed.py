import argparse
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ARCHIVE_SUFFIXES = ('.a', '.lib')
UNDEFINED_TYPES = {'U', 'u', 'w', 'v'}


def load_target(build_root, path):
  path = os.path.realpath(path)
  with open(os.path.join(build_root, 'meson-info', 'intro-targets.json')) as f:
    for target in json.load(f):
      if any(os.path.realpath(p) == path for p in target['filename']):
        return target
  sys.exit(f'embed: {path} not found in meson introspection data')


def link_inputs(build_root, target):
  section = next(s for s in target['target_sources'] if 'linker' in s)
  archives, system = [], []
  params = iter(section['parameters'])
  for arg in params:
    if arg == '-framework':
      system += [arg, next(params)]
    elif arg.startswith('-l') or arg in ('-pthread', '-no-pie'):
      system.append(arg)
    elif arg.endswith(ARCHIVE_SUFFIXES):
      path = os.path.realpath(os.path.join(build_root, arg))
      if os.path.isfile(path) and path.startswith(build_root + os.sep):
        archives.append(path)
      else:
        system.append(arg)
  return list(dict.fromkeys(archives)), list(dict.fromkeys(system))


def compile_section(target):
  return next(s for s in target['target_sources'] if s.get('language') == 'c')


def defined_symbols(nm, archive):
  result = subprocess.run([nm, '-g', '-P', '--defined-only', archive], capture_output=True, text=True)
  if result.returncode != 0:
    return None
  symbols = set()
  for line in result.stdout.splitlines():
    fields = line.split()
    if len(fields) >= 2 and not fields[0].endswith(':') and len(fields[1]) == 1 and fields[1] not in UNDEFINED_TYPES:
      symbols.add(fields[0])
  return symbols


def select_archives(nm, archives):
  selected, seen = [], set()
  for archive in archives:
    symbols = defined_symbols(nm, archive)
    if selected and symbols is not None and symbols <= seen:
      continue
    selected.append(archive)
    seen |= symbols or set()
  return selected


def merge_archives(style, tool, output, archives):
  if style == 'libtool':
    subprocess.run([tool, '-static', '-o', output] + archives, check=True)
  elif style == 'lib':
    subprocess.run([tool, '/NOLOGO', '/OUT:' + output] + archives, check=True)
  else:
    script = ''.join(f'ADDLIB {a}\n' for a in archives)
    subprocess.run([tool, '-M'], input=f'CREATE {output}\n{script}SAVE\nEND\n', text=True, check=True)


def header_closure(build_root, section, headers):
  flags = [p for p in section['parameters'] if p.startswith(('-I', '-D', '-std='))]
  out = subprocess.run(
    section['compiler'] + flags + ['-x', 'c', '-MM', '-MG'] + headers,
    cwd=build_root, capture_output=True, text=True, check=True,
  ).stdout
  deps = set()
  for token in out.replace('\\\n', ' ').split():
    if token.endswith(':'):
      continue
    path = os.path.realpath(os.path.join(build_root, token))
    if os.path.isfile(path):
      deps.add(path)
  return deps


INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)([<"])([^>"]+)[>"]', re.M)


def include_dirs(build_root, section):
  dirs = [os.path.realpath(os.path.join(build_root, p[2:])) for p in section['parameters'] if p.startswith('-I')]
  return list(dict.fromkeys(dirs))


def dist_name(dirs, header):
  owners = [d for d in dirs if header.startswith(d + os.sep)]
  if not owners:
    sys.exit(f'embed: {header} is not under any include directory')
  return 'ant/' + os.path.relpath(header, max(owners, key=len)).replace(os.sep, '/')


def resolve_include(dirs, including, quote, name):
  search = ([os.path.dirname(including)] if quote == '"' else []) + dirs
  for d in search:
    path = os.path.join(d, name)
    if os.path.isfile(path):
      return os.path.realpath(path)
  return None


def namespaced(dirs, names, header):
  with open(header, errors='surrogateescape') as f:
    text = f.read()

  def rewrite(m):
    target = resolve_include(dirs, header, m.group(2), m.group(3))
    return f'{m.group(1)}<{names[target]}>' if target in names else m.group(0)

  return INCLUDE_RE.sub(rewrite, text)


def write_pc(path, version, defines, system_libs):
  cflags = ' '.join(['-I${includedir}'] + [shlex.quote(d) for d in defines])
  libs = ' '.join(['-L${libdir}', '-lant'] + [shlex.quote(a) for a in system_libs])
  with open(path, 'w') as f:
    f.write(
      'prefix=${pcfiledir}/../..\n'
      'libdir=${prefix}/lib\n'
      'includedir=${prefix}/include\n\n'
      'Name: libant\n'
      "Description: javascript for 🐜's\n"
      f'Version: {version}\n'
      f'Cflags: {cflags}\n'
      f'Libs: {libs}\n'
    )


def bundle(args):
  build_root = os.path.realpath(args.build_root)
  source_root = os.path.realpath(args.source_root)
  exe = load_target(build_root, args.exe)
  lib = load_target(build_root, args.lib)
  section = compile_section(lib)

  archives, system_libs = link_inputs(build_root, exe)
  libant = os.path.realpath(args.lib)
  archives = [libant] + [a for a in archives if a != libant]
  archives = select_archives(args.nm, archives)

  out = args.out
  shutil.rmtree(out, ignore_errors=True)
  os.makedirs(os.path.join(out, 'lib', 'pkgconfig'))

  archive_name = 'ant.lib' if args.archiver_style == 'lib' else 'libant.a'
  with tempfile.TemporaryDirectory(dir=out) as tmp:
    staged = os.path.join(tmp, archive_name)
    merge_archives(args.archiver_style, args.archiver, staged, archives)
    os.replace(staged, os.path.join(out, 'lib', archive_name))

  headers = sorted(glob.glob(os.path.join(source_root, 'include', '**', '*.h'), recursive=True))
  if not headers:
    sys.exit(f'embed: no headers under {source_root}/include')
  dirs = include_dirs(build_root, section)
  names, placed = {}, {}
  for header in sorted(header_closure(build_root, section, headers)):
    name = dist_name(dirs, header)
    if placed.setdefault(name, header) != header:
      sys.exit(f'embed: {header} and {placed[name]} both map to {name}')
    names[header] = name

  text = []
  for header, name in names.items():
    content = namespaced(dirs, names, header)
    dest = os.path.join(out, 'include', name)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'w', errors='surrogateescape') as f:
      f.write(content)
    text.append(content)

  umbrella = names[os.path.join(source_root, 'include', 'ant.h')]
  with open(os.path.join(out, 'include', 'ant.h'), 'w') as f:
    f.write(f'#include <{umbrella}>\n')

  used = set(re.findall(r'\b\w+\b', '\n'.join(text)))
  defines = [p for p in section['parameters'] if p.startswith('-D') and p != '-DNDEBUG' and p[2:].split('=', 1)[0] in used]
  write_pc(os.path.join(out, 'lib', 'pkgconfig', 'libant.pc'), args.version, defines, system_libs)


def pc_fields(path):
  variables = {'pcfiledir': os.path.dirname(os.path.realpath(path))}
  fields = {}

  def expand(value):
    for name, val in variables.items():
      value = value.replace('${' + name + '}', val)
    return value

  with open(path) as f:
    for line in f:
      line = line.rstrip('\n')
      if '=' in line and ':' not in line.split('=', 1)[0]:
        name, value = line.split('=', 1)
        variables[name] = expand(value)
      elif ': ' in line:
        name, value = line.split(': ', 1)
        fields[name] = expand(value)
  return {name: shlex.split(fields[name]) for name in ('Cflags', 'Libs')}


def example(args):
  fields = pc_fields(os.path.join(args.dist, 'lib', 'pkgconfig', 'libant.pc'))
  obj = args.out + '.o'
  subprocess.run(args.cc + ['-std=gnu23'] + args.extra + fields['Cflags'] + ['-c', args.source, '-o', obj], check=True)
  subprocess.run(args.cxx + args.extra + [obj] + fields['Libs'] + ['-o', args.out], check=True)
  os.remove(obj)


def main():
  parser = argparse.ArgumentParser()
  sub = parser.add_subparsers(dest='command', required=True)

  p = sub.add_parser('bundle')
  p.add_argument('--build-root', required=True)
  p.add_argument('--source-root', required=True)
  p.add_argument('--exe', required=True)
  p.add_argument('--lib', required=True)
  p.add_argument('--nm', required=True)
  p.add_argument('--archiver', required=True)
  p.add_argument('--archiver-style', choices=['libtool', 'mri', 'lib'], required=True)
  p.add_argument('--version', required=True)
  p.add_argument('--out', required=True)

  p = sub.add_parser('example')
  p.add_argument('--dist', required=True)
  p.add_argument('--source', required=True)
  p.add_argument('--out', required=True)
  p.add_argument('--cc', action='append', required=True)
  p.add_argument('--cxx', action='append', required=True)
  p.add_argument('--extra', action='append', default=[])

  args = parser.parse_args()
  bundle(args) if args.command == 'bundle' else example(args)


if __name__ == '__main__':
  main()
