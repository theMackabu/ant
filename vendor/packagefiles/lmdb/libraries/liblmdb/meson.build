cc = meson.get_compiler('c')

src = files('mdb.c', 'midl.c')

if host_machine.system() == 'windows'
  threads_dep = []
else
  threads_dep = dependency('threads')
endif

incdir = include_directories('.')

lmdb = library(
  'lmdb',
  src,
  vs_module_defs: '../../lmdb.def',
  dependencies: threads_dep,
  install: true,
)

install_headers('lmdb.h')

lmdb_dep = declare_dependency(
  include_directories: incdir,
  link_with: lmdb,
)

# Programs require POSIX APIs from unistd.h
if cc.has_header(
  'unistd.h',
  required: get_option('programs'),
)
  progs = ['mdb_stat', 'mdb_copy', 'mdb_dump', 'mdb_load']

  foreach p : progs
    exe = executable(
      p,
      '@0@.c'.format(p),
      dependencies: lmdb_dep,
      install: true,
    )
    meson.override_find_program(p, exe)
    install_man('@0@.1'.format(p))
  endforeach
endif

if not get_option('tests')
  subdir_done()
endif

tests = ['mtest', 'mtest2', 'mtest3', 'mtest4', 'mtest5']

python = import('python').find_installation()

snippet = ('''
import sys, shutil, subprocess, os
dir = "testdb"

try:
  # lmdb expects an empty db
  shutil.rmtree(dir)
except FileNotFoundError:
  pass

os.mkdir(dir)
subprocess.check_call([sys.argv[1]])
  '''
    )

foreach t : tests
  exe = executable(
    t,
    '@0@.c'.format(t),
    dependencies: [lmdb_dep],
  )
  test(
    t,
    python,
    args: ['-c', snippet, exe],
    is_parallel: false,
  )
endforeach
