project(
  'sqlite3',
  'c',
  version: '3.54.1',
  license: 'blessing',
  default_options: [
    'warning_level=0',
    'default_library=static',
  ],
)

sqlite3_feature_args = [
  '-DSQLITE_ENABLE_SESSION',
  '-DSQLITE_ENABLE_PREUPDATE_HOOK',
  '-DSQLITE_ENABLE_COLUMN_METADATA',
]

sqlite3_args = sqlite3_feature_args + [
  '-Os',
  '-DSQLITE_DQS=0',
  '-DSQLITE_THREADSAFE=2',
  '-DSQLITE_DEFAULT_MEMSTATUS=0',
  '-DSQLITE_ENABLE_MATH_FUNCTIONS',
  '-DSQLITE_OMIT_AUTOINIT',
  '-DSQLITE_OMIT_DEPRECATED',
  '-DSQLITE_OMIT_SHARED_CACHE',
  '-DSQLITE_USE_ALLOCA',
]

sqlite3_inc = include_directories('.')

sqlite3_lib = static_library(
  'sqlite3',
  files('sqlite3.c'),
  c_args: sqlite3_args,
  include_directories: sqlite3_inc,
  build_by_default: false,
  install: false,
)

sqlite3_dep = declare_dependency(
  link_with: sqlite3_lib,
  include_directories: sqlite3_inc,
  compile_args: sqlite3_feature_args,
)

sqlite3_headers_dep = declare_dependency(
  include_directories: sqlite3_inc,
  compile_args: sqlite3_feature_args,
)
