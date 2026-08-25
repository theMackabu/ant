node = find_program('node', required: true)

builtin_files = files(run_command(
  python, files('../sources.py'), src_root / 'sources.json', 'builtins', src_root,
  check: true
).stdout().strip().split())

builtins_h = custom_target(
  'builtins',
  input: builtin_files,
  output: ['builtin_bundle_data.h', 'snapshot_data.h'],
  depends: tools_npm_ci,
  command: [
    node,
    src_root / 'src' / 'tools' / 'gen_builtins.js',
    src_root / 'src' / 'builtins',
    '@OUTPUT0@',
    '@OUTPUT1@',
    src_root / 'src' / 'builtins' / 'bootstrap' / 'index.ts',
    'VERSION=' + ant_version,
    'GIT_HASH=' + git_longhash,
    'BUILD_TIMESTAMP=' + timestamp,
    'TARGET=' + target_triple,
    'HOST=' + host_machine.system(),
    '--',
    '@INPUT@',
  ],
)
