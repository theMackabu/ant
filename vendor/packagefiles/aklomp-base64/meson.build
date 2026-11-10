project(
  'aklomp-base64',
  'c',
  version: '0.5.3',
  license: 'BSD-2-Clause',
  meson_version: '>=0.59.0',
  default_options: ['warning_level=3'],
)

pkg = import('pkgconfig')

cc = meson.get_compiler('c')
tests_opt = get_option('tests').disable_auto_if(meson.is_subproject())

c_args = ['-DBASE64_EXPORTS']
config = configuration_data()

if get_option('default_library') == 'static'
  c_args += ['-DBASE64_STATIC_DEFINE']
endif

is_x86 = host_machine.cpu_family() in ['x86', 'x86_64']
is_arm = host_machine.cpu_family() in ['arm', 'aarch64']
config.set10('BASE64_WITH_NEON64', host_machine.cpu_family() == 'aarch64')

arch_flags = {}

if cc.get_id() == 'msvc'
  if is_x86
    arch_flags += {
      'SSSE3': [],
      'SSE41': [],
      'SSE42': [],
      'AVX': ['/arch:AVX'],
      'AVX2': ['/arch:AVX2'],
      'AVX512': ['/arch:AVX512'],
    }
  elif is_arm
    arch_flags += {
      'NEON32': [],
    }
  endif
else
  if is_x86
    arch_flags += {
      'SSSE3': ['-mssse3'],
      'SSE41': ['-msse4.1'],
      'SSE42': ['-msse4.2'],
      'AVX': ['-mavx'],
      'AVX2': ['-mavx2'],
      'AVX512': ['-mavx512vl', '-mavx512vbmi'],
    }
  elif is_arm
    arch_flags += {
      'NEON32': ['-mfpu=neon'],
    }
  endif
endif

foreach feature, flags : arch_flags
  present = cc.has_multi_arguments(flags)
  config.set10('BASE64_WITH_' + feature, present)
  if not present
    arch_flags += {
      feature: [],
    }
  endif
endforeach

cfg = configure_file(
  input: 'cmake/config.h.in',
  output: 'config.h',
  configuration: config,
  format: 'cmake',
)

codecs = []

codecs += [
  static_library(
    'aklomp-base64-generic',
    'lib/arch/generic/codec.c',
    'lib/tables/tables.c',
    c_args: c_args,
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-ssse3',
    'lib/arch/ssse3/codec.c',
    c_args: c_args + arch_flags.get('SSSE3', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-sse41',
    'lib/arch/sse41/codec.c',
    c_args: c_args + arch_flags.get('SSE41', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-sse42',
    'lib/arch/sse42/codec.c',
    c_args: c_args + arch_flags.get('SSE42', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-avx',
    'lib/arch/avx/codec.c',
    c_args: c_args + arch_flags.get('AVX', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-avx2',
    'lib/arch/avx2/codec.c',
    c_args: c_args + arch_flags.get('AVX2', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-avx512',
    'lib/arch/avx512/codec.c',
    c_args: c_args + arch_flags.get('AVX512', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-neon32',
    'lib/arch/neon32/codec.c',
    c_args: c_args + arch_flags.get('NEON32', []),
    override_options: ['c_std=c99'],
  ),
  static_library(
    'aklomp-base64-neon64',
    'lib/arch/neon64/codec.c',
    c_args: c_args,
    override_options: ['c_std=c99'],
  ),
]

src = files('lib/codec_choose.c', 'lib/lib.c')

inc = include_directories('include')

libbase64 = library(
  'base64',
  src,
  c_args: c_args,
  include_directories: inc,
  link_with: codecs,
  install: true,
  version: meson.project_version(),
  override_options: ['c_std=c99'],
  gnu_symbol_visibility: 'hidden',
)

base64_dep = declare_dependency(
  include_directories: inc,
  link_with: libbase64,
)

install_headers('include/libbase64.h')

meson.override_dependency('base64', base64_dep)

pkg.generate(
  libbase64,
  description: 'Fast Base64 stream encoder/decoder in C99, with SIMD acceleration',
  url: 'https://github.com/aklomp/base64',
)

if tests_opt.allowed()
  test_src = files('test/codec_supported.c', 'test/test_base64.c')

  test_c_args = cc.get_supported_arguments(['-Wno-strict-prototypes'])
  test_exe = executable(
    'base64-test',
    test_src,
    c_args: test_c_args,
    include_directories: inc,
    link_with: libbase64,
    override_options: ['c_std=c99'],
    build_by_default: false,
  )
  test('base64', test_exe)
endif
