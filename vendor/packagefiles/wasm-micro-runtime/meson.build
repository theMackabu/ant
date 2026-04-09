project('wasm-micro-runtime', ['c', 'cpp'], default_options: ['c_std=gnu11', 'warning_level=0'])

cc = meson.get_compiler('c')

if host_machine.system() == 'windows'
  wamr_platform = 'windows'
elif host_machine.system() == 'darwin'
  wamr_platform = 'darwin'
else
  wamr_platform = 'linux'
endif

common_c_args = [
  '-DNDEBUG',
  '-DBH_MALLOC=wasm_runtime_malloc',
  '-DBH_FREE=wasm_runtime_free',
  '-DWASM_ENABLE_INTERP=1',
  '-DWASM_ENABLE_FAST_INTERP=0',
  '-DWASM_ENABLE_AOT=0',
  '-DWASM_ENABLE_JIT=0',
  '-DWASM_ENABLE_FAST_JIT=0',
  '-DWASM_ENABLE_LIBC_BUILTIN=0',
  '-DWASM_ENABLE_LIBC_WASI=1',
  '-DWASM_ENABLE_MULTI_MODULE=0',
  '-DWASM_ENABLE_BULK_MEMORY=1',
  '-DWASM_ENABLE_BULK_MEMORY_OPT=1',
  '-DWASM_ENABLE_SHARED_MEMORY=0',
  '-DWASM_ENABLE_REF_TYPES=1',
  '-DWASM_ENABLE_CALL_INDIRECT_OVERLONG=1',
  '-DWASM_ENABLE_MINI_LOADER=0',
  '-DWASM_DISABLE_HW_BOUND_CHECK=1',
  '-DWASM_DISABLE_STACK_HW_BOUND_CHECK=1',
  '-DWASM_DISABLE_WAKEUP_BLOCKING_OP=0',
  '-DWASM_ENABLE_SIMD=0',
  '-DWASM_ENABLE_MODULE_INST_CONTEXT=1',
  '-DWASM_GLOBAL_HEAP_SIZE=' + (10 * 1024 * 1024).to_string(),
]

if wamr_platform == 'windows'
  common_c_args += [
    '-DBH_PLATFORM_WINDOWS',
    '-DHAVE_STRUCT_TIMESPEC',
    '-D_WINSOCK_DEPRECATED_NO_WARNINGS',
    '-DWASM_HAVE_MREMAP=0',
  ]
elif wamr_platform == 'darwin'
  common_c_args += ['-DBH_PLATFORM_DARWIN', '-DWASM_HAVE_MREMAP=0']
else
  common_c_args += ['-DBH_PLATFORM_LINUX', '-DWASM_HAVE_MREMAP=1', '-D_GNU_SOURCE']
endif

if host_machine.cpu_family() == 'aarch64'
  common_c_args += ['-DBUILD_TARGET_AARCH64', '-DBUILD_TARGET=AARCH64']
elif host_machine.cpu_family() == 'x86_64'
  common_c_args += ['-DBUILD_TARGET_X86_64', '-DBUILD_TARGET=X86_64']
else
  error('Unsupported WAMR target: ' + host_machine.cpu_family())
endif

wamr_inc = include_directories(
  '.',
  'core',
  'core/iwasm/include',
  'core/iwasm/common',
  'core/iwasm/interpreter',
  'core/iwasm/libraries/libc-wasi',
  'core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/include',
  'core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/src',
  'core/shared/mem-alloc',
  'core/shared/utils',
  'core/shared/platform/' + wamr_platform,
  'core/shared/platform/include',
  'core/shared/platform/common/libc-util',
  'core/shared/platform/common/memory',
  'core/shared/platform/common/posix',
)

if wamr_platform == 'windows'
  wamr_sources = files(
    'core/shared/platform/windows/platform_init.c',
    'core/shared/platform/windows/win_clock.c',
    'core/shared/platform/windows/win_file.c',
    'core/shared/platform/windows/win_malloc.c',
    'core/shared/platform/windows/win_memmap.c',
    'core/shared/platform/windows/win_socket.c',
    'core/shared/platform/windows/win_thread.c',
    'core/shared/platform/windows/win_time.c',
    'core/shared/platform/windows/win_util.c',
    'core/shared/platform/windows/win_atomic.cpp',
    'core/shared/platform/common/libc-util/libc_errno.c',
    'core/shared/platform/common/memory/mremap.c',
  )
else
  wamr_sources = files(
  'core/shared/platform/' + wamr_platform + '/platform_init.c',
  'core/shared/platform/common/libc-util/libc_errno.c',
  'core/shared/platform/common/posix/posix_blocking_op.c',
  'core/shared/platform/common/posix/posix_clock.c',
  'core/shared/platform/common/posix/posix_malloc.c',
  'core/shared/platform/common/posix/posix_memmap.c',
  'core/shared/platform/common/posix/posix_sleep.c',
  'core/shared/platform/common/posix/posix_thread.c',
  'core/shared/platform/common/posix/posix_time.c',
  )
endif

if wamr_platform == 'darwin'
  wamr_sources += files('core/shared/platform/common/memory/mremap.c')
endif

wamr_sources += files(
  'core/shared/utils/bh_assert.c',
  'core/shared/utils/bh_bitmap.c',
  'core/shared/utils/bh_common.c',
  'core/shared/utils/bh_hashmap.c',
  'core/shared/utils/bh_leb128.c',
  'core/shared/utils/bh_list.c',
  'core/shared/utils/bh_log.c',
  'core/shared/utils/bh_queue.c',
  'core/shared/utils/bh_vector.c',
  'core/shared/utils/runtime_timer.c',
  'core/shared/mem-alloc/mem_alloc.c',
  'core/shared/mem-alloc/ems/ems_alloc.c',
  'core/shared/mem-alloc/ems/ems_gc.c',
  'core/shared/mem-alloc/ems/ems_hmu.c',
  'core/shared/mem-alloc/ems/ems_kfc.c',
  'core/iwasm/common/wasm_application.c',
  'core/iwasm/common/wasm_blocking_op.c',
  'core/iwasm/common/wasm_c_api.c',
  'core/iwasm/common/wasm_exec_env.c',
  'core/iwasm/common/wasm_loader_common.c',
  'core/iwasm/common/wasm_memory.c',
  'core/iwasm/common/wasm_native.c',
  'core/iwasm/common/wasm_runtime_common.c',
  'core/iwasm/common/wasm_shared_memory.c',
  'core/iwasm/interpreter/wasm_loader.c',
  'core/iwasm/interpreter/wasm_runtime.c',
  'core/iwasm/interpreter/wasm_interp_classic.c',
  'core/iwasm/libraries/libc-wasi/libc_wasi_wrapper.c',
  'core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/src/posix.c',
  'core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/src/str.c',
  'core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/src/random.c',
  'core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/src/blocking_op.c',
)

if host_machine.cpu_family() == 'aarch64'
  if wamr_platform == 'darwin'
    wamr_sources += files('core/iwasm/common/arch/invokeNative_aarch64.s')
  else
    wamr_sources += files('core/iwasm/common/arch/invokeNative_aarch64.s')
  endif
elif host_machine.cpu_family() == 'x86_64'
  if wamr_platform == 'windows'
    wamr_sources += files('core/iwasm/common/arch/invokeNative_mingw_x64.s')
  else
    wamr_sources += files('core/iwasm/common/arch/invokeNative_em64.s')
  endif
else
  wamr_sources += files('core/iwasm/common/arch/invokeNative_general.c')
endif

if wamr_platform != 'windows'
  wamr_sources += files(
    'core/shared/platform/common/posix/posix_file.c',
    'core/shared/platform/common/posix/posix_socket.c',
  )
endif

wamr_lib = static_library(
  'wamr',
  wamr_sources,
  include_directories: wamr_inc,
  c_args: common_c_args,
  cpp_args: common_c_args,
)

wamr_link_deps = [dependency('threads')]
if host_machine.system() != 'windows'
  wamr_link_deps += [cc.find_library('m', required: true)]
endif

wamr_dep = declare_dependency(
  link_with: wamr_lib,
  include_directories: wamr_inc,
  compile_args: [
    '-DWASM_ENABLE_INTERP=1',
    '-DWASM_ENABLE_LIBC_WASI=1',
    '-DWASM_ENABLE_MODULE_INST_CONTEXT=1',
    '-DWASM_ENABLE_BULK_MEMORY=1',
    '-DWASM_ENABLE_REF_TYPES=1',
    '-DWASM_ENABLE_SHARED_MEMORY=0',
    '-DWASM_ENABLE_SIMD=0',
    '-DWASM_ENABLE_AOT=0',
    '-DWASM_ENABLE_MULTI_MODULE=0',
    '-DWASM_ENABLE_TAGS=0',
    '-DWASM_CONFIGURABLE_BOUNDS_CHECKS=0',
  ],
  dependencies: wamr_link_deps,
)
