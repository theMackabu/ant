project('ant', ['c', 'cpp'], default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'cpp_std=gnu++23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

python = find_program('python3')
fs = import('fs')
is_static = get_option('static_link')

src_root = meson.project_source_root()
include = include_directories('include')
sandbox_backend_include = include_directories(
  'src/sandbox/backends',
  'src/sandbox/backends/shared/include',
)

sandbox_shared_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:shared',
  check: true
).stdout().strip().split())

sandbox_backend_sources = []
darwin_hvf_supported = host_machine.system() == 'darwin' and host_machine.cpu_family() == 'aarch64'
if host_machine.system() == 'darwin'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:darwin',
    check: true
  ).stdout().strip().split())
  if darwin_hvf_supported
    sandbox_backend_sources += files(run_command(
      python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:darwin_hvf',
      check: true
    ).stdout().strip().split())
    sandbox_backend_sources += sandbox_shared_sources
  endif
elif host_machine.system() == 'linux'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:linux',
    check: true
  ).stdout().strip().split())
  sandbox_backend_sources += sandbox_shared_sources
elif host_machine.system() == 'windows'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:windows',
    check: true
  ).stdout().strip().split())
endif

subdir('meson')

messages_h = custom_target('messages_h',
  input: 'src/core/messages.toml',
  output: 'messages.h',
  command: [python, files('meson/messages.py'), '@INPUT@'],
  capture: true,
)

theme_h = custom_target('theme_h',
  input: 'src/highlight/theme.toml',
  output: 'theme.h',
  command: [python, files('meson/theme.py'), '@INPUT@'],
  capture: true,
)

lib_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'engine',
  check: true
).stdout().strip().split())

pkg_lib = custom_target(
  'pkg_zig',
  output: 'libpkg.a',
  command: [
    'sh', '-c',
    '"$ZIG" build --build-file "$PKG_ZIG_DIR/build.zig" --prefix "$PKG_BUILD_DIR" && cp "$PKG_BUILD_DIR/lib/$PKG_LIB_NAME" "@OUTPUT@"'
  ],
  env: {
    'ZIG': zig.full_path(),
    'ANT_VERSION': ant_version,

    'PKG_ZIG_DIR': pkg_zig_dir, 'PKG_BUILD_DIR': pkg_build_dir,
    'LMDB_INCLUDE': lmdb_include_path, 'ZLIB_INCLUDE': zlib_include_path,
    'LIBUV_INCLUDE': libuv_include_path, 'YYJSON_INCLUDE': yyjson_include_path,

    'PKG_TARGET': host_machine.cpu_family() + '-' + host_machine.system(),
    'PKG_LIB_NAME': host_machine.system() == 'windows' ? 'pkg.lib' : 'libpkg.a',
  },
  build_by_default: true,
  build_always_stale: true
)

pkg_dep = declare_dependency(
  link_with: [pkg_lib],
  include_directories: include,
  dependencies: [lmdb_dep, tlsuv_dep, libuv_dep, nghttp2_dep]
)

libant = static_library(
  'ant',
  lib_sources + sandbox_backend_sources + [
    builtin_bundle_h,
    snapshot_h,
    messages_h,
    theme_h
  ],
  install: true,
  include_directories: [include, sandbox_backend_include],
  dependencies: ant_deps
)

libant_dep = declare_dependency(
  link_with: libant,
  include_directories: [include, sandbox_backend_include],
  dependencies: ant_deps + [pkg_dep]
)

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

ant_link_args = link_args
if host_machine.system() == 'windows'
  ant_link_args += ['-Wl,--stack,134217728']
elif host_machine.system() == 'darwin'
  ant_link_args += ['-Wl,-stack_size,0x2000000']
else
  ant_link_args += ['-Wl,-z,stack-size=134217728']
endif
if get_option('linker_map')
  ant_linkmap = meson.project_build_root() + '/ant.linkmap'
  if host_machine.system() == 'darwin'
    ant_link_args += ['-Wl,-map,' + ant_linkmap]
  else
    ant_link_args += ['-Wl,-Map=' + ant_linkmap]
  endif
endif

ant_exe = executable(
  'ant',
  files('src/main.c') + [messages_h],
  dependencies: libant_dep,
  link_args: ant_link_args,
  export_dynamic: true
)

if host_machine.system() == 'darwin' and get_option('codesign')
  codesign = find_program('codesign')
  custom_target(
    'ant_codesign',
    input: files('meson/ant.entitlements'),
    output: 'ant.codesign.stamp',
    command: [
      'sh', '-c',
      '"$1" --force --sign - --entitlements "$2" "$3" && touch "$4"',
      'ant-codesign',
      codesign,
      '@INPUT0@',
      ant_exe,
      '@OUTPUT@',
    ],
    depends: ant_exe,
    build_by_default: true,
  )
endif

# move to meson/tests
executable(
  'tlsuv-boringssl-smoke',
  files('tests/tlsuv_boringssl_smoke.c'),
  dependencies: [tlsuv_dep, libuv_dep, llhttp, zlib_dep] + (host_machine.system() == 'darwin' ? [security_dep] : []),
  build_by_default: false
)

executable(
  'bench-allocator',
  files('tests/bench-allocator.c'),
  dependencies: [dependency('threads')] + allocator_deps,
  build_by_default: false
)

executable(
  'test-http-protocols',
  files('tests/test_http_protocols.c'),
  dependencies: libant_dep,
  build_by_default: false
)

embed_example = get_option('embed_example')
embed_example_dist = 'packages/libant/dist'

if not embed_example.disabled()
  if fs.is_dir(meson.project_source_root() / embed_example_dist)
    executable(
      'embed-example',
      files('examples/embed/embed.c'),
      include_directories: include_directories(embed_example_dist),
      dependencies: libant_dep,
      build_by_default: false
    )
  elif embed_example.enabled()
    error('embed_example requires ' + embed_example_dist + '; run packages/libant/build.sh first')
  endif
endif

if darwin_hvf_supported
  executable(
    'test-sandbox-net-packets',
    files('tests/test_sandbox_net_packets.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-virtio-blk',
    files('tests/test_sandbox_virtio_blk.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-virtio-rng',
    files('tests/test_sandbox_virtio_rng.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-pci',
    files('tests/test_sandbox_pci.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-mmio',
    files('tests/test_sandbox_mmio.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-9p',
    files('tests/test_sandbox_9p.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-vsock-queue',
    files('tests/test_sandbox_vsock_queue.c'),
    dependencies: libant_dep,
    build_by_default: false
  )
endif
