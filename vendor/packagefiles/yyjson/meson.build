project(
  'yyjson',
  'c',
  version: '0.13.0',
  license: 'MIT',
  meson_version: '>=0.54.0',
)

c_args = []
feature_flags = {
  'reader': 'YYJSON_DISABLE_READER',
  'writer': 'YYJSON_DISABLE_WRITER',
  'incr-reader': 'YYJSON_DISABLE_INCR_READER',
  'utils': 'YYJSON_DISABLE_UTILS',
  'fast-fp-conv': 'YYJSON_DISABLE_FAST_FP_CONV',
  'non-standard-json': 'YYJSON_DISABLE_NON_STANDARD',
  'utf8-validation': 'YYJSON_DISABLE_UTF8_VALIDATION',
  'unaligned-memory-access': 'YYJSON_DISABLE_UNALIGNED_MEMORY_ACCESS',
}

foreach feature, flag : feature_flags
  if get_option(feature).disabled()
    c_args += f'-D@flag@'
  endif
endforeach

yyjson_inc = include_directories('src')

yyjson_lib = library(
  'yyjson',
  sources: files('src/yyjson.c'),
  c_args: [c_args],
  include_directories: yyjson_inc,
  version: meson.project_version(),
)

yyjson_dep = declare_dependency(
  include_directories: yyjson_inc,
  link_with: yyjson_lib,
)

meson.override_dependency('yyjson', yyjson_dep)
