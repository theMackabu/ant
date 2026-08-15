temporal_link_deps = []
temporal_opt = get_option('temporal')

if temporal_opt.enabled() or temporal_opt.auto()
  temporal_capi_project = subproject('temporal_capi', required: temporal_opt)
  if temporal_capi_project.found()
    temporal_capi_source = temporal_capi_project.get_variable('temporal_capi_source_dir')
    temporal_capi_inputs = temporal_capi_project.get_variable('temporal_capi_build_inputs')
    temporal_cpu = host_machine.cpu_family()
    temporal_cc_syntax = cc.get_argument_syntax()

    if temporal_cpu != 'aarch64' and temporal_cpu != 'x86_64'
      error('Temporal does not support CPU family ' + temporal_cpu)
    endif

    if host_machine.system() == 'darwin'
      temporal_target = temporal_cpu + '-apple-darwin'
    elif host_machine.system() == 'windows'
      if temporal_cc_syntax == 'msvc'
        temporal_target = temporal_cpu + '-pc-windows-msvc'
      elif temporal_cc_syntax == 'gcc'
        temporal_target = temporal_cpu + '-pc-windows-gnu'
      else
        error('Temporal cannot determine the Windows ABI from compiler argument syntax ' + temporal_cc_syntax)
      endif
    elif host_machine.system() == 'linux'
      temporal_glibc = cc.get_define(
        '__GLIBC__',
        prefix: '#include <features.h>',
      )
      if temporal_glibc != ''
        temporal_target = temporal_cpu + '-unknown-linux-gnu'
      else
        temporal_target = temporal_cpu + '-unknown-linux-musl'
      endif
    else
      error('Temporal does not support ' + host_machine.system())
    endif

    temporal_build_cpu = build_machine.cpu_family()
    temporal_build_cc = meson.get_compiler('c', native: true)
    temporal_build_cc_syntax = temporal_build_cc.get_argument_syntax()

    if temporal_build_cpu != 'aarch64' and temporal_build_cpu != 'x86_64'
      error('Temporal bootstrap does not support CPU family ' + temporal_build_cpu)
    endif

    if build_machine.system() == 'darwin'
      temporal_rustup_host = temporal_build_cpu + '-apple-darwin'
    elif build_machine.system() == 'windows'
      if temporal_build_cc_syntax == 'msvc'
        temporal_rustup_host = temporal_build_cpu + '-pc-windows-msvc'
      elif temporal_build_cc_syntax == 'gcc'
        temporal_rustup_host = temporal_build_cpu + '-pc-windows-gnu'
      else
        error('Temporal cannot determine the bootstrap Windows ABI from compiler argument syntax ' + temporal_build_cc_syntax)
      endif
    elif build_machine.system() == 'linux'
      temporal_build_glibc = temporal_build_cc.get_define(
        '__GLIBC__',
        prefix: '#include <features.h>',
      )
      if temporal_build_glibc != ''
        temporal_rustup_host = temporal_build_cpu + '-unknown-linux-gnu'
      else
        temporal_rustup_host = temporal_build_cpu + '-unknown-linux-musl'
      endif
    else
      error('Temporal bootstrap does not support ' + build_machine.system())
    endif

    temporal_lib_name = temporal_target.endswith('-windows-msvc') ? 'ant_temporal.lib' : 'libant_temporal.a'
    temporal_features = host_machine.system() == 'windows' ? 'tzdb-embedded' : 'fs-tzdb'
    temporal_include = temporal_capi_source / 'bindings/c'

    message(
      'Temporal Rust target: ' + temporal_target + ' (compiler: ' + cc.get_id() +
      '/' + temporal_cc_syntax +
      ', bootstrap host: ' + temporal_rustup_host + ')'
    )

    temporal_lib = custom_target('temporal_staticlib',
      input: files(
        src_root / 'src/temporal/Cargo.toml',
        src_root / 'src/temporal/Cargo.lock',
        src_root / 'src/temporal/src/lib.rs',
        src_root / 'src/temporal/rust-toolchain.toml',
      ) + temporal_capi_inputs,
      output: temporal_lib_name,
      command: [
        python, files(src_root / 'meson/temporal.py'),
        '--crate', src_root / 'src/temporal',
        '--temporal-capi', temporal_capi_source,
        '--cache', src_root / '.cache/temporal-rust',
        '--rustup-host', temporal_rustup_host,
        '--target', temporal_target,
        '--out', '@OUTPUT@',
        '--features', temporal_features,
      ],
    )

    ant_deps += [declare_dependency(
      compile_args: ['-DANT_HAVE_TEMPORAL=1', '-I' + temporal_include],
    )]
    temporal_link_deps += [declare_dependency(link_with: temporal_lib)]
  endif
endif
