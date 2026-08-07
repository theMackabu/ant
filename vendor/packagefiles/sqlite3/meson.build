project('sqlite3', 'c',
  license: 'blessing',
  version: '3.53.4',
  meson_version: '>=0.56.0',
)

cc = meson.get_compiler('c')
libm = cc.find_library('m', required: false)
threads_dep = dependency('threads')

# Size-tuned build: the recommended compile-time options from
# https://www.sqlite.org/compile.html plus omissions for features the
# ant:sqlite module does not expose. WAL and core SQL stay intact.
sqlite_args = [
  '-DSQLITE_DQS=0',
  '-DSQLITE_THREADSAFE=2',
  '-DSQLITE_DEFAULT_MEMSTATUS=0',
  '-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1',
  '-DSQLITE_LIKE_DOESNT_MATCH_BLOBS',
  '-DSQLITE_MAX_EXPR_DEPTH=0',
  '-DSQLITE_OMIT_DEPRECATED',
  '-DSQLITE_OMIT_PROGRESS_CALLBACK',
  '-DSQLITE_OMIT_SHARED_CACHE',
  '-DSQLITE_OMIT_LOAD_EXTENSION',
  '-DSQLITE_OMIT_AUTOINIT',
  '-DSQLITE_USE_ALLOCA',
  '-DSQLITE_ENABLE_MATH_FUNCTIONS',
  '-DSQLITE_ENABLE_COLUMN_METADATA',
]

sqlite3_lib = static_library('sqlite3', 'sqlite3.c',
  c_args: sqlite_args,
  dependencies: [libm, threads_dep],
  install: false,
)

sqlite3_dep = declare_dependency(
  link_with: sqlite3_lib,
  include_directories: include_directories('.'),
)
