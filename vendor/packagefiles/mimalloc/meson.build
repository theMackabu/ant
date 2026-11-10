project('mimalloc', 'c', version: '3.6.1', default_options: [
  'c_std=c11',
  'warning_level=0',
  'default_library=static',
])

cc = meson.get_compiler('c')

mimalloc_include = include_directories('include', 'src')

mimalloc_c_args = [
  '-DMI_MALLOC_OVERRIDE',
  '-DMI_STATIC_LIB',
  '-DMI_BUILD_RELEASE',
  '-DMI_CMAKE_BUILD_TYPE=release',
]

if host_machine.system() == 'darwin'
  mimalloc_c_args += [
    '-DMI_OSX_ZONE=1',
    '-DMI_OSX_INTERPOSE=1',
  ]
endif

if host_machine.system() == 'windows'
  mimalloc_c_args += ['-DMI_WIN_NOREDIRECT=1']
else
  if cc.has_header('unistd.h')
    mimalloc_c_args += ['-DMI_HAS_UNISTDH=1']
  endif
  if cc.has_header('execinfo.h')
    mimalloc_c_args += ['-DMI_HAS_EXECINFOH=1']
  endif
endif

if cc.get_id() == 'clang'
  mimalloc_c_args += cc.get_supported_arguments('-mno-outline')
  if host_machine.cpu_family() == 'aarch64' and cc.has_multi_arguments('-Xclang', '-fno-split-cold-code')
    mimalloc_c_args += ['-Xclang', '-fno-split-cold-code']
  endif
endif

mimalloc_lib = static_library(
  'mimalloc',
  files('src/static.c'),
  include_directories: mimalloc_include,
  c_args: mimalloc_c_args,
  dependencies: dependency('threads'),
  pic: true,
  install: false,
)

mimalloc_dep = declare_dependency(
  include_directories: mimalloc_include,
  link_whole: mimalloc_lib,
  dependencies: dependency('threads'),
)
