node = find_program('node', required: true)

builtin_files = files(run_command(
  python, files('../sources.py'), src_root / 'sources.json', 'builtins', src_root,
  check: true
).stdout().strip().split())

builtin_bundle_h = custom_target(
  'builtin_bundle',
  input: builtin_files,
  output: 'builtin_bundle_data.h',
  command: [
    node,
    src_root / 'src' / 'tools' / 'gen_builtin_bundle.js',
    src_root / 'src' / 'builtins',
    '@OUTPUT@',
    '@INPUT@',
  ],
)
