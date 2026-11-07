rust_link_deps = []
temporal_opt = get_option('temporal')
encoding_rs_opt = get_option('encoding_rs')
rust_want_temporal = temporal_opt.allowed()
rust_want_encoding = encoding_rs_opt.allowed()

if rust_want_temporal or rust_want_encoding
  temporal_capi_project = subproject('temporal_capi', required: temporal_opt.enabled() or encoding_rs_opt.enabled())
  if temporal_capi_project.found()
    temporal_capi_source = temporal_capi_project.get_variable('temporal_capi_source_dir')
    temporal_capi_inputs = temporal_capi_project.get_variable('temporal_capi_build_inputs')
    rust_cpu = host_machine.cpu_family()
    rust_cc_syntax = cc.get_argument_syntax()

    if host_machine.system() == 'wasi'
      rust_target = 'wasm32-wasip1'
    elif rust_cpu != 'aarch64' and rust_cpu != 'x86_64'
      error('Rust does not support CPU family ' + rust_cpu)
    elif host_machine.system() == 'darwin'
      rust_target = rust_cpu + '-apple-darwin'
    elif host_machine.system() == 'windows'
      if rust_cc_syntax == 'msvc'
        rust_target = rust_cpu + '-pc-windows-msvc'
      elif rust_cc_syntax == 'gcc'
        rust_windows_abi = rust_cpu == 'aarch64' ? 'gnullvm' : 'gnu'
        rust_target = rust_cpu + '-pc-windows-' + rust_windows_abi
      else
        error('Rust cannot determine the Windows ABI from compiler argument syntax ' + rust_cc_syntax)
      endif
    elif host_machine.system() == 'linux'
      rust_glibc = cc.get_define(
        '__GLIBC__',
        prefix: '#include <features.h>',
      )
      if rust_glibc != ''
        rust_target = rust_cpu + '-unknown-linux-gnu'
      else
        rust_target = rust_cpu + '-unknown-linux-musl'
      endif
    else
      error('Rust does not support ' + host_machine.system())
    endif

    rust_build_cpu = build_machine.cpu_family()
    rust_build_cc = meson.get_compiler('c', native: true)
    rust_build_cc_syntax = rust_build_cc.get_argument_syntax()

    if rust_build_cpu != 'aarch64' and rust_build_cpu != 'x86_64'
      error('Rust bootstrap does not support CPU family ' + rust_build_cpu)
    endif

    if build_machine.system() == 'darwin'
      rust_rustup_host = rust_build_cpu + '-apple-darwin'
    elif build_machine.system() == 'windows'
      if rust_build_cc_syntax == 'msvc'
        rust_rustup_host = rust_build_cpu + '-pc-windows-msvc'
      elif rust_build_cc_syntax == 'gcc'
        rust_rustup_abi = rust_build_cpu == 'aarch64' ? 'msvc' : 'gnu'
        rust_rustup_host = rust_build_cpu + '-pc-windows-' + rust_rustup_abi
      else
        error('Rust cannot determine the bootstrap Windows ABI from compiler argument syntax ' + rust_build_cc_syntax)
      endif
    elif build_machine.system() == 'linux'
      rust_build_glibc = rust_build_cc.get_define(
        '__GLIBC__',
        prefix: '#include <features.h>',
      )
      if rust_build_glibc != ''
        rust_rustup_host = rust_build_cpu + '-unknown-linux-gnu'
      else
        rust_rustup_host = rust_build_cpu + '-unknown-linux-musl'
      endif
    else
      error('Rust bootstrap does not support ' + build_machine.system())
    endif

    rust_lib_name = rust_target.endswith('-windows-msvc') ? 'ant_rust.lib' : 'libant_rust.a'
    temporal_include = temporal_capi_source / 'bindings/c'
    rust_features = []
    if rust_want_temporal
      rust_features += ['windows', 'wasi'].contains(host_machine.system()) ? 'tzdb-embedded' : 'fs-tzdb'
    endif
    if rust_want_encoding
      rust_features += 'encoding'
    endif

    message(
      'Rust target: ' + rust_target + ' (compiler: ' + cc.get_id() +
      '/' + rust_cc_syntax +
      ', bootstrap host: ' + rust_rustup_host +
      ', features: ' + ','.join(rust_features) + ')'
    )

    rust_lib = custom_target('rust_staticlib',
      input: files(
        src_root / 'src/rust/Cargo.toml',
        src_root / 'src/rust/Cargo.lock',
        src_root / 'src/rust/src/lib.rs',
        src_root / 'src/rust/rust-toolchain.toml',
      ) + temporal_capi_inputs,
      output: rust_lib_name,
      build_by_default: true,
      command: [
        python, files(src_root / 'meson/rust.py'),
        '--crate', src_root / 'src/rust',
        '--temporal-capi', temporal_capi_source,
        '--cache', src_root / '.cache/rust',
        '--rustup-host', rust_rustup_host,
        '--target', rust_target,
        '--out', '@OUTPUT@',
        '--features', ','.join(rust_features),
      ],
    )

    if rust_want_temporal
      temporal_args = ['-DANT_HAVE_TEMPORAL=1', '-I' + temporal_include]
      if ['windows', 'wasi'].contains(host_machine.system())
        temporal_args += '-DANT_TEMPORAL_EMBEDDED_TZDB=1'
      endif
      ant_deps += [declare_dependency(compile_args: temporal_args)]
    endif
    if rust_want_encoding
      ant_deps += [declare_dependency(compile_args: ['-DANT_HAVE_ENCODING_RS=1'])]
    endif
    rust_link_deps += [declare_dependency(link_with: rust_lib)]
  endif
endif
