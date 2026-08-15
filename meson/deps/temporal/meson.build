temporal_link_deps = []
temporal_opt = get_option('temporal')

if temporal_opt.enabled() or temporal_opt.auto()
  temporal_capi_project = subproject('temporal_capi', required: temporal_opt)
  if temporal_capi_project.found()
    temporal_capi_source = temporal_capi_project.get_variable('temporal_capi_source_dir')
    temporal_capi_inputs = temporal_capi_project.get_variable('temporal_capi_build_inputs')
    temporal_cpu = host_machine.cpu_family()
    temporal_cc_target = run_command(
      cc.cmd_array(), '-dumpmachine',
      check: true,
    ).stdout().strip()

    if temporal_cpu != 'aarch64' and temporal_cpu != 'x86_64'
      error('Temporal does not support CPU family ' + temporal_cpu)
    endif

    if host_machine.system() == 'darwin'
      temporal_target = temporal_cpu + '-apple-darwin'
    elif host_machine.system() == 'windows'
      if temporal_cc_target.contains('msvc')
        temporal_target = temporal_cpu + '-pc-windows-msvc'
      elif temporal_cc_target.contains('gnu') or temporal_cc_target.contains('mingw')
        temporal_target = temporal_cpu + '-pc-windows-gnu'
      else
        error('Temporal cannot determine the Windows ABI from compiler target ' + temporal_cc_target)
      endif
    elif host_machine.system() == 'linux'
      if temporal_cc_target.contains('musl')
        temporal_target = temporal_cpu + '-unknown-linux-musl'
      elif temporal_cc_target.contains('gnu')
        temporal_target = temporal_cpu + '-unknown-linux-gnu'
      else
        error('Temporal cannot determine the Linux ABI from compiler target ' + temporal_cc_target)
      endif
    else
      error('Temporal does not support ' + host_machine.system())
    endif

    temporal_lib_name = temporal_target.endswith('-windows-msvc') ? 'ant_temporal.lib' : 'libant_temporal.a'
    temporal_features = host_machine.system() == 'windows' ? 'tzdb-embedded' : 'fs-tzdb'
    temporal_include = temporal_capi_source / 'bindings/c'

    message('Temporal Rust target: ' + temporal_target + ' (compiler: ' + temporal_cc_target + ')')

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
        '--rustup-host', temporal_target,
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
