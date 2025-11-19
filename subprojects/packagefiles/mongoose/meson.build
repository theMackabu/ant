project('mongoose', 'c', version: '7.0')

mongoose_inc = include_directories('.')

mongoose_lib = static_library(
  'mongoose',
  'mongoose.c',
  include_directories: mongoose_inc,
  install: false
)

mongoose_dep = declare_dependency(
  link_with: mongoose_lib,
  include_directories: mongoose_inc
)

meson.override_dependency('mongoose', mongoose_dep)
