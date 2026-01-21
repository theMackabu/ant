# Snapshot generation
# Include with: subdir('meson') or subdir('../meson')

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
    'VERSION=' + version_conf.get('ANT_VERSION'),
    'GIT_HASH=' + version_conf.get('ANT_GIT_HASH'),
    'BUILD_TIMESTAMP=' + version_conf.get('ANT_BUILD_TIMESTAMP'),
    'TARGET=' + version_conf.get('ANT_TARGET_TRIPLE'),
    'MBEDTLS=' + (tls_lib == 'mbedtls').to_string(),
    'HOST=' + host_machine.system()
  ],
  build_by_default: true
)
