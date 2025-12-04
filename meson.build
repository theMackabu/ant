project('ant', 'c', default_options: [
  'optimization=2',
  'c_std=c23',
  'default_library=static'
])

cmake = import('cmake')
include = include_directories('include')

module_files = run_command('sh', '-c', 
  'cd "$MESON_SOURCE_ROOT" && ls src/modules/*.c',
  check: true
).stdout().strip().split()

sources = files(
  'src/main.c',
  'src/ant.c',
  'src/repl.c',
  'src/runtime.c',
  'src/snapshot.c',
) + files(module_files)

zlib_dep = dependency('zlib', required: true)
llhttp = dependency('llhttp', required: true)
openssl_dep = dependency('openssl', required: true)
libsodium_dep = dependency('libsodium', required: true)

if host_machine.system() == 'darwin'
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'], required: true)
endif

tlsuv_opts = cmake.subproject_options()
tlsuv_opts.append_compile_args('c', [
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-bitwise-op-parentheses',
  '-Wno-sometimes-uninitialized'
])

tlsuv_opts.add_cmake_defines({'BUILD_TESTING': 'OFF'})
tlsuv_dep = cmake.subproject('tlsuv', options: tlsuv_opts).dependency('tlsuv')

uthash_dep = subproject('uthash').get_variable('uthash_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')
uuidv7_dep = subproject('uuidv7').get_variable('uuidv7_dep')
mongoose_dep = subproject('mongoose').get_variable('mongoose_dep')
argtable3_dep = subproject('argtable3').get_variable('argtable3_dep')
minicoro_dep = subproject('minicoro').get_variable('minicoro_dep')

libuv_dep = subproject('libuv', default_options: [
  'build_tests=false', 
  'build_benchmarks=false'
]).get_variable('libuv_dep')

git = find_program('git', required: false)
if git.found()
  git_commit = run_command(git, 'rev-parse', '--short=10', 'HEAD', check: false)
  if git_commit.returncode() == 0
    git_hash = git_commit.stdout().strip()
  else
    git_hash = 'unknown'
  endif
else
  git_hash = 'unknown'
endif

build_date = run_command('date', '+%Y-%m-%d', check: true).stdout().strip()

version_conf = configuration_data()
version_conf.set('ANT_VERSION', '0.0.6.29')
version_conf.set('ANT_GIT_HASH', git_hash)
version_conf.set('ANT_BUILD_DATE', build_date)

config_h = configure_file(
  input: 'include/config.h.in',
  output: 'config.h',
  configuration: version_conf
)

build_include = include_directories('.')
add_project_arguments('-D JS_DUMP', '-Wno-unused-function', language: 'c')

gen_snapshot = executable(
  'gen_snapshot',
  files('src/tools/gen_snapshot.c'),
  include_directories: [include, build_include],
  c_args: ['-DANT_SNAPSHOT_GENERATOR'],
  native: true
)

snapshot_h = custom_target(
  'snapshot',
  input: 'src/core/index.js',
  output: 'snapshot_data.h',
  command: [
    gen_snapshot, 
    '@INPUT@', 
    '@OUTPUT@',
    'VERSION=' + version_conf.get('ANT_VERSION'),
    'GIT_HASH=' + version_conf.get('ANT_GIT_HASH'),
    'BUILD_DATE=' + version_conf.get('ANT_BUILD_DATE')
  ],
  build_by_default: true
)

ant_deps = [
  llhttp, mongoose_dep,
  libuv_dep, argtable3_dep,
  tlsuv_dep, libsodium_dep,
  yyjson_dep, minicoro_dep,
  uuidv7_dep, openssl_dep,
  zlib_dep, uthash_dep
]

if host_machine.system() == 'darwin'
  ant_deps += [security_dep]
endif

executable(
  'ant',
  sources + [snapshot_h],
  include_directories: [include, build_include],
  dependencies: ant_deps
)
