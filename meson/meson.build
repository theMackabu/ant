message('Build configuration:')
message('  buildtype: ' + get_option('buildtype'))
message('  optimization: ' + get_option('optimization'))
message('  debug: ' + (get_option('debug') ? 'true' : 'false'))
message('  b_lto: ' + (get_option('b_lto') ? 'true' : 'false'))
message('  strip: ' + (get_option('strip') ? 'true' : 'false'))
message('  linker_map: ' + (get_option('linker_map') ? 'true' : 'false'))

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

if get_option('buildtype') != 'release'
  warning('Building in non-release mode. For best performance, use --buildtype=release')
endif

add_project_arguments(
  '-DNO_EXECUTE_PERMISSION',
  '-Wshadow',
  '-Wdouble-promotion',
  '-Wformat=2',
  '-Wundef',
  '-Wconversion',
  '-Wno-sign-conversion',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  '-Wno-deprecated-declarations',
  language: 'c'
)

if host_machine.system() == 'darwin'
  deployment_flag = '-mmacosx-version-min=15.0'
  add_project_arguments(deployment_flag, language: ['c', 'cpp'])
  add_project_link_arguments(deployment_flag, language: ['c', 'cpp'])
endif

if host_machine.system() == 'linux'
  add_project_arguments('-fno-pie', language: ['c', 'cpp'])
  add_project_link_arguments('-no-pie', language: ['c', 'cpp'])
endif

if host_machine.system() == 'windows'
  # MinGW headers default to an older Windows API level; Ant uses Win8+/Win10
  # APIs (GetCurrentThreadStackLimits, CreateFile2, ...).
  add_project_arguments('-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', language: ['c', 'cpp'])
endif

native_tuning = get_option('native_tuning')
native_tuning_flag = ''
if ['x86', 'x86_64'].contains(host_machine.cpu_family())
  native_tuning_flag = '-march=native'
else
  native_tuning_flag = '-mcpu=native'
endif
native_tuning_supported = (
  cc.has_argument(native_tuning_flag) and
  cpp.has_argument(native_tuning_flag)
)

if native_tuning.enabled()
  if not native_tuning_supported
    error('native_tuning requested but compiler does not support ' + native_tuning_flag)
  endif
  add_project_arguments(native_tuning_flag, language: ['c', 'cpp'])
  message('  native_tuning: enabled (' + native_tuning_flag + ')')
elif native_tuning.auto() and native_tuning_supported and get_option('buildtype') == 'release'
  add_project_arguments(native_tuning_flag, language: ['c', 'cpp'])
  message('  native_tuning: auto enabled (' + native_tuning_flag + ')')
else
  message('  native_tuning: disabled')
endif

pgo = get_option('pgo')
pgo_generate_dir = get_option('pgo_generate_dir')
pgo_file = 'ant-' + host_machine.system() + '-' + host_machine.cpu_family() + '.profdata'
pgo_profile = src_root / 'meson' / 'pgo' / 'profiles' / pgo_file
pgo_profile_exists = fs.is_file(pgo_profile)

pgo_c_id = cc.get_id()
pgo_cpp_id = cpp.get_id()

pgo_compiler_supported = (
  (pgo_c_id == 'clang' or pgo_c_id == 'appleclang' or pgo_c_id == 'apple-clang') and
  (pgo_cpp_id == 'clang' or pgo_cpp_id == 'appleclang' or pgo_cpp_id == 'apple-clang')
)

pgo_auto_compatible = get_option('buildtype') == 'release' and get_option('b_sanitize') == 'none'
pgo_use = false

if pgo_generate_dir != ''
  if not pgo.disabled()
    error('pgo_generate_dir requires -Dpgo=disabled; cannot generate and use PGO in the same build')
  endif
  if not pgo_compiler_supported
    error('pgo_generate_dir requires Clang/Apple Clang, found c=' + pgo_c_id + ', cpp=' + pgo_cpp_id)
  endif
  pgo_generate_args = ['-fprofile-generate=' + pgo_generate_dir]
  add_project_arguments(pgo_generate_args, language: ['c', 'cpp'])
  add_project_link_arguments(pgo_generate_args, language: ['c', 'cpp'])
  message('  pgo: generate (' + pgo_generate_dir + ')')
elif pgo.enabled()
  if not pgo_profile_exists
    error('PGO profile requested but not found: ' + pgo_profile)
  endif
  if not pgo_compiler_supported
    error('PGO profile requested but requires Clang/Apple Clang, found c=' + pgo_c_id + ', cpp=' + pgo_cpp_id)
  endif
  pgo_use = true
elif pgo.auto() and pgo_profile_exists and pgo_compiler_supported and pgo_auto_compatible
  pgo_use = true
endif

if pgo_use
  pgo_compile_args = [
    '-fprofile-use=' + pgo_profile,
    '-Wno-profile-instr-unprofiled',
    '-Wno-profile-instr-out-of-date',
  ]
  add_project_arguments(pgo_compile_args, language: ['c', 'cpp'])
  add_project_link_arguments('-fprofile-use=' + pgo_profile, language: ['c', 'cpp'])
  message('  pgo: enabled (' + pgo_profile + ')')
elif pgo_profile_exists
  message('  pgo: profile found but not used (' + pgo_profile + ')')
else
  message('  pgo: no profile for ' + host_machine.system() + '-' + host_machine.cpu_family())
endif

subdir('deps')
subdir('version')

npm = find_program('npm', required: true)
tools_npm_ci = custom_target(
  'tools_npm_ci',
  input: [
    src_root / 'src' / 'tools' / 'package.json',
    src_root / 'src' / 'tools' / 'npm-shrinkwrap.json',
  ],
  output: 'tools_npm_ci.stamp',
  command: [
    python,
    files('npm_ci.py'),
    npm.full_path(),
    src_root / 'src' / 'tools',
    '@OUTPUT@',
  ],
)

subdir('snapshot')
subdir('builtins')
