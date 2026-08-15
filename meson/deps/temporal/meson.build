temporal_link_deps = []
temporal_opt = get_option('temporal')

if temporal_opt.enabled() or temporal_opt.auto()
  temporal_capi_project = subproject('temporal_capi', required: temporal_opt)
  if temporal_capi_project.found()
    temporal_capi_source = temporal_capi_project.get_variable('temporal_capi_source_dir')
    temporal_capi_inputs = temporal_capi_project.get_variable('temporal_capi_build_inputs')

    if host_machine.system() == 'darwin'
      temporal_target = host_machine.cpu_family() + '-apple-darwin'
    elif host_machine.system() == 'windows'
      temporal_target = host_machine.cpu_family() == 'aarch64' ? 'aarch64-pc-windows-msvc' : 'x86_64-pc-windows-msvc'
    else
      temporal_target = host_machine.cpu_family() + '-unknown-linux-gnu'
    endif

    temporal_lib_name = host_machine.system() == 'windows' ? 'ant_temporal.lib' : 'libant_temporal.a'
    temporal_features = host_machine.system() == 'windows' ? 'tzdb-embedded' : 'fs-tzdb'
    temporal_include = temporal_capi_source / 'bindings/c'

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
