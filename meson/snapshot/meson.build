node = find_program('node', required: true)

core_files = run_command('sh', '-c', 
  'find ' + src_root / 'src' / 'core' + ' -name "*.js" ! -name "index.js" | sort',
  check: true
).stdout().strip().split()

snapshot_h = custom_target(
  'snapshot',
  input: [src_root / 'src' / 'core' / 'index.ts'] + files(core_files),
  output: 'snapshot_data.h',
  command: [
    node,
    src_root / 'src' / 'tools' / 'gen_snapshot.js',
    '@INPUT0@', 
    '@OUTPUT@',
    'VERSION=' + ant_version,
    'GIT_HASH=' + git_hash,
    'BUILD_TIMESTAMP=' + timestamp,
    'TARGET=' + target_triple,
    'MBEDTLS=' + (tls_lib == 'mbedtls').to_string(),
    'HOST=' + host_machine.system(),
  ],
)