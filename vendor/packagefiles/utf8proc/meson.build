project(
  'utf8proc',
  'c',
  version: '2.12.0',
  license: 'MIT',
  default_options: 'c_std=c99',
  meson_version: '>=0.59.0',
)

## NOTE: update these versions on updates, if they change, see upstream Cmake defintions

# This is the ABI version number, which may differ from the API (project) version number
utfproc_abi_version = '3.3.0'

# the unicode version this supports, used for tests
unicode_version = '18.0.0'

cc = meson.get_compiler('c')
fs = import('fs')
pkg = import('pkgconfig')

if get_option('default_library') == 'static'
  add_project_arguments(
    '-DUTF8PROC_STATIC',
    language: 'c',
  )
endif

add_project_arguments(
  '-DUTF8PROC_EXPORTS',
  language: 'c',
)


utf8proc_lib = library(
  'utf8proc',
  'utf8proc.c',
  gnu_symbol_visibility: 'hidden',
  install: true,
  version: utfproc_abi_version,
)

utf8proc_dep = declare_dependency(
  include_directories: include_directories('.'),
  link_with: utf8proc_lib,
)

pkg.generate(
  utf8proc_lib,
  description: 'UTF8 processing',
  extra_cflags: '-DUTF8PROC_EXPORTS',
  name: 'libutf8proc',
)

install_headers('utf8proc.h')

curl = find_program(
  'curl',
  required: get_option('tests'),
)

if get_option('tests').require(curl.found()).allowed()

  _testdefs = {
    'case'         : '',
    'custom'       : '',
    'graphemetest' : 'auxiliary/GraphemeBreakTest.txt',
    'iterate'      : '',
    'misc'         : '',
    'normtest'     : 'NormalizationTest.txt',
    'printproperty': '',
    'valid'        : '',
  }

  if cc.has_header_symbol(
    'wchar.h',
    'wcwidth',
    args: '-D_XOPEN_SOURCE',
  )
    _testdefs += {
      'charwidth': '',
    }
  endif

  foreach _test, _data : _testdefs
    if _data == ''
      _args = []
      _depends = []
    else
      _args = fs.name(_data)
      _depends = custom_target(
        _args,
        build_by_default: false,
        command: [
          curl,
          '--continue-at',
          '-',
          '--output',
          '@OUTPUT@',
          ('https://www.unicode.org/Public/' + unicode_version + '/ucd') / _data,
        ],
        console: true,
        output: _args,
      )
    endif
    test(
      _test,
      executable(
        _test,
        'test/tests.c',
        'test/@0@.c'.format(_test),
        dependencies: utf8proc_dep,
      ),
      args: _args,
      depends: _depends,
      workdir: meson.current_build_dir(),
    )
  endforeach

endif
