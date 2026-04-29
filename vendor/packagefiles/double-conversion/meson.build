project('double-conversion', 'cpp',
  default_options: [
    'cpp_std=c++17',
    'warning_level=0',
    'default_library=static',
  ]
)

double_conversion_inc = include_directories('.')

double_conversion_lib = static_library(
  'double-conversion',
  files(
    'double-conversion/bignum.cc',
    'double-conversion/bignum-dtoa.cc',
    'double-conversion/cached-powers.cc',
    'double-conversion/double-to-string.cc',
    'double-conversion/fast-dtoa.cc',
    'double-conversion/fixed-dtoa.cc',
    'double-conversion/string-to-double.cc',
    'double-conversion/strtod.cc',
  ),
  include_directories: double_conversion_inc,
  install: false,
)

double_conversion_dep = declare_dependency(
  link_with: double_conversion_lib,
  include_directories: double_conversion_inc,
)
