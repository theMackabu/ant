node = find_program('node', required: true)

core_files = files(run_command(
  python, files('../sources.py'), src_root / 'sources.json', 'core', src_root,
  check: true
).stdout().strip().split())

snapshot_h = custom_target(
  'snapshot',
  input: core_files,
  output: 'snapshot_data.h',
  command: [
    node,
    src_root / 'src' / 'tools' / 'gen_snapshot.js',
    src_root / 'src' / 'core' / 'index.ts',
    '@OUTPUT@',
    'VERSION=' + ant_version,
    'GIT_HASH=' + git_hash,
    'BUILD_TIMESTAMP=' + timestamp,
    'TARGET=' + target_triple,
    'MBEDTLS=' + (tls_lib == 'mbedtls').to_string(),
    'HOST=' + host_machine.system(),
  ],
)
