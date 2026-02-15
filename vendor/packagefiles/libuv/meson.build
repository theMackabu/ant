project('libuv', 'c',
  version: '1.52.0',
  default_options: [
    'warning_level=0', 'werror=false',
    meson.version().version_compare('>=1.3.0') ? 'c_std=gnu11,c11' : 'c_std=gnu11',
  ],
  meson_version: '>=0.49.0',
)

# get compiler type
cc = meson.get_compiler('c')
cc_msvc = cc.get_argument_syntax() == 'msvc'

# get system type
win32 = host_machine.system() == 'windows'
gnu = host_machine.system() == 'gnu'
linux = host_machine.system() == 'linux'
android = host_machine.system() == 'android'
apple = host_machine.system() == 'darwin'
freebsd = host_machine.system() == 'freebsd'
netbsd = host_machine.system() == 'netbsd'
openbsd = host_machine.system() == 'openbsd'
dragonfly = host_machine.system() == 'dragonfly'
haiku = host_machine.system() == 'haiku'
sunos = host_machine.system() == 'sunos'
qnx = cc.get_define('__QNX__') != ''
aix = cc.get_define('_AIX') != ''
os390 = (false  # TODO: detect
)
os400 = (false  # TODO: detect
)
message('System: ' + host_machine.system())

if cc_msvc
  add_project_arguments(
    '-D_CRT_SECURE_NO_WARNINGS',
    cc.get_supported_arguments('-Wno-deprecated-declarations'),
    language: 'c',
  )
endif

# get options
libuv_build_tests = get_option('build_tests')
libuv_build_bench = get_option('build_benchmarks')
libuv_qemu = get_option('build_for_qemu')
libuv_asan = get_option('b_sanitize').contains('address')
if not libuv_build_tests
  libuv_build_bench = false
endif

# variable declaration
libuv_cargs = []
libuv_libs = []
libuv_test_src = files()
libuv_test_libs = []
libuv_headers = files(
  'include/uv/errno.h',
  'include/uv/threadpool.h',
  'include/uv/version.h',
)

# option-depending config
if libuv_qemu
  libuv_cargs += '-D__QEMU__=1'
endif

if libuv_asan and cc.get_argument_syntax() == 'gcc'
  libuv_cargs += '-D__ASAN__=1'
endif

if cc_msvc
  libuv_cargs += '/we4013'
endif

# compiler flags
if cc_msvc
  cc_msvc_cargs_all = [
    '/wd4100',  # no_unused_parameter
    '/wd4127',  # no conditional constant
    '/wd4201',  # no_nonstandard
    '/wd4206',  # no_nonstandard_empty_tu
    '/wd4210',  # no_nonstandard_file_scope
    '/wd4232',  # no_nonstandard_nonstatic_dlimport
    '/wd4456',  # no_hides_local
    '/wd4457',  # no_hides_param
    '/wd4459',  # no_hides_global
    '/wd4706',  # no_conditional_assignment
    '/wd4496',  # no_unsafe
  ]

  foreach flag : cc_msvc_cargs_all
    if cc.has_argument(flag)
      libuv_cargs += flag
    endif
  endforeach
endif

cc_other_cargs_all = [
  '-fno-strict-aliasing',  # f_strict_aliasing
]
foreach flag : cc_other_cargs_all
  if cc.has_argument(flag)
    libuv_cargs += flag
  endif
endforeach

# basic sources
libuv_src = files(
  'src/fs-poll.c',
  'src/idna.c',
  'src/inet.c',
  'src/random.c',
  'src/strscpy.c',
  'src/strtok.c',
  'src/thread-common.c',
  'src/threadpool.c',
  'src/timer.c',
  'src/uv-common.c',
  'src/uv-data-getter-setters.c',
  'src/version.c',
)

# system-depending config
if win32
  libuv_cargs += [
    '-DWIN32_LEAN_AND_MEAN',
    '-D_WIN32_WINNT=0x0A00',
    '-D_CRT_DECLARE_NONSTDC_NAMES=0',
  ]
  libuv_libs += [
    'psapi',
    'user32',
    'advapi32',
    'iphlpapi',
    'userenv',
    'ws2_32',
    'dbghelp',
    'ole32',
    'uuid',
    'shell32',
  ]
  libuv_src += files(
    'src/win/async.c',
    'src/win/core.c',
    'src/win/detect-wakeup.c',
    'src/win/dl.c',
    'src/win/error.c',
    'src/win/fs-event.c',
    'src/win/fs.c',
    'src/win/getaddrinfo.c',
    'src/win/getnameinfo.c',
    'src/win/handle.c',
    'src/win/loop-watcher.c',
    'src/win/pipe.c',
    'src/win/poll.c',
    'src/win/process-stdio.c',
    'src/win/process.c',
    'src/win/signal.c',
    'src/win/snprintf.c',
    'src/win/stream.c',
    'src/win/tcp.c',
    'src/win/thread.c',
    'src/win/tty.c',
    'src/win/udp.c',
    'src/win/util.c',
    'src/win/winapi.c',
    'src/win/winsock.c',
  )
  libuv_test_src += files('src/win/snprintf.c', 'test/runner-win.c')
  libuv_headers += files('include/uv/tree.h', 'include/uv/win.h')
else
  libuv_cargs += ['-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE_SOURCE']
  libuv_src += files(
    'src/unix/async.c',
    'src/unix/core.c',
    'src/unix/dl.c',
    'src/unix/fs.c',
    'src/unix/getaddrinfo.c',
    'src/unix/getnameinfo.c',
    'src/unix/loop-watcher.c',
    'src/unix/loop.c',
    'src/unix/pipe.c',
    'src/unix/poll.c',
    'src/unix/process.c',
    'src/unix/random-devurandom.c',
    'src/unix/signal.c',
    'src/unix/stream.c',
    'src/unix/tcp.c',
    'src/unix/thread.c',
    'src/unix/tty.c',
    'src/unix/udp.c',
  )
  libuv_test_src += files('test/runner-unix.c')
  libuv_headers += files('include/uv/unix.h')
endif

if aix
  libuv_cargs += [
    '-D_ALL_SOURCE',
    '-D_LINUX_SOURCE_COMPAT',
    '-D_THREAD_SAFE',
    '-D_XOPEN_SOURCE=500',
    '-D_REENTRANT',
    '-DHAVE_SYS_AHAFS_EVPRODS_H',
  ]
  libuv_libs += 'perfstat'
  libuv_src += files('src/unix/aix-common.c', 'src/unix/aix.c')
  libuv_headers += files('include/uv/aix.h')
endif

if android
  libuv_cargs += '-D_GNU_SOURCE'
  libuv_libs += 'dl'
  libuv_src += files(
    'src/unix/linux.c',
    'src/unix/procfs-exepath.c',
    'src/unix/random-getentropy.c',
    'src/unix/random-getrandom.c',
    'src/unix/random-sysctl-linux.c',
  )
endif

if apple or android or linux
  libuv_src += files('src/unix/proctitle.c')
endif

if dragonfly or freebsd
  libuv_src += files('src/unix/freebsd.c')
endif

if dragonfly or freebsd or netbsd or openbsd
  libuv_src += files('src/unix/bsd-proctitle.c', 'src/unix/posix-hrtime.c')
  libuv_headers += files('include/uv/bsd.h')
endif

if apple or dragonfly or freebsd or netbsd or openbsd
  libuv_src += files('src/unix/bsd-ifaddrs.c', 'src/unix/kqueue.c')
endif

if freebsd
  libuv_src += files('src/unix/random-getrandom.c')
endif

if apple or openbsd
  libuv_src += files('src/unix/random-getentropy.c')
endif

if apple
  libuv_cargs += [
    '-D_DARWIN_UNLIMITED_SELECT=1',
    '-D_DARWIN_USE_64_BIT_INODE=1',
  ]
  libuv_src += files(
    'src/unix/darwin-proctitle.c',
    'src/unix/darwin.c',
    'src/unix/fsevents.c',
  )
  libuv_headers += files('include/uv/darwin.h')
endif

if gnu
  libuv_cargs += [
    '-D_GNU_SOURCE',
    '-D_POSIX_C_SOURCE=200112',
    '-D_XOPEN_SOURCE=500',
  ]
  libuv_libs += ['dl']
  libuv_src += files(
    'src/unix/bsd-ifaddrs.c',
    'src/unix/hurd.c',
    'src/unix/no-fsevents.c',
    'src/unix/no-proctitle.c',
    'src/unix/posix-hrtime.c',
    'src/unix/posix-poll.c',
  )
endif

if linux
  libuv_cargs += ['-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200112']
  libuv_libs += ['dl', 'rt']
  libuv_src += files(
    'src/unix/linux.c',
    'src/unix/procfs-exepath.c',
    'src/unix/random-getrandom.c',
    'src/unix/random-sysctl-linux.c',
  )
  libuv_headers += files('include/uv/linux.h')
endif

if netbsd
  libuv_src += files('src/unix/netbsd.c')
  libuv_libs += 'kvm'
endif

if openbsd
  libuv_src += files('src/unix/openbsd.c')
endif

if os390
  # TODO
endif

if os400
  # TODO
endif

if sunos
  libuv_cargs += ['-D__EXTENSIONS__', '-D_XOPEN_SOURCE=500']
  libuv_libs += ['kstat', 'nsl', 'sendfile', 'socket']
  libuv_src += files('src/unix/no-proctitle.c', 'src/unix/sunos.c')
  libuv_headers += files('include/uv/sunos.h')
endif

if haiku
  libuv_cargs += '-D_BSD_SOURCE'
  libuv_libs += ['bsd', 'network']
  libuv_src += files(
    'src/unix/bsd-ifaddrs.c',
    'src/unix/haiku.c',
    'src/unix/no-fsevents.c',
    'src/unix/no-proctitle.c',
    'src/unix/posix-hrtime.c',
    'src/unix/posix-poll.c',
  )
  libuv_headers += files('include/uv/posix.h')
endif

if qnx
  libuv_libs += 'socket'
  libuv_src += files(
    'src/unix/bsd-ifaddrs.c',
    'src/unix/no-fsevents.c',
    'src/unix/no-proctitle.c',
    'src/unix/posix-hrtime.c',
    'src/unix/posix-poll.c',
    'src/unix/qnx.c',
  )
endif

if (apple
or dragonfly
or freebsd
or linux
or netbsd
or openbsd)
  libuv_test_libs += 'util'
  libuv_libs += 'm'
endif

# convert libs to dependencies
libuv_deps = [dependency('threads')]
foreach lib : libuv_libs
  libuv_deps += cc.find_library(
    lib,
    required: true,
  )
endforeach
libuv_test_deps = []
if libuv_build_tests
  foreach lib : libuv_test_libs
    libuv_test_deps += cc.find_library(
      lib,
      required: true,
    )
  endforeach
endif

# include directories
libuv_public_inc = include_directories('include')
libuv_private_inc = include_directories('src')
libuv_inc = [libuv_public_inc, libuv_private_inc]

# libraries and respective dependency declarations
libuv_test_lflags = []
if cc_msvc
  # TODO: workaround for msvc, because of __declspec(dllexport)
  if get_option('default_library') == 'both'
    error('default_library=both is not supported with MSVC')
  elif get_option('default_library') == 'shared'
    libuv_cargs += '-DBUILDING_UV_SHARED=1'
  endif
endif
if libuv_qemu and get_option('default_library') == 'static'
  libuv_test_lflags += '-static'
endif

libuv = library(
  'uv',
  libuv_src,
  c_args: libuv_cargs,
  dependencies: libuv_deps,
  include_directories: libuv_inc,
  version: '1.0.0',
  install: true,
)
libuv_dep = declare_dependency(
  dependencies: libuv_deps,
  link_with: libuv,
  include_directories: libuv_public_inc,
)
import('pkgconfig').generate(
  libuv,
  name: 'libuv',
  description: 'multi-platform support library with a focus on asynchronous I/O.',
  url: 'http://libuv.org/',
)

install_headers('include/uv.h')
install_headers(
  libuv_headers,
  subdir: 'uv',
)

# benchmark & test config
if libuv_build_bench
  libuv_bench_src = libuv_test_src
  libuv_bench_src += files(
    'test/benchmark-async-pummel.c',
    'test/benchmark-async.c',
    'test/benchmark-fs-stat.c',
    'test/benchmark-getaddrinfo.c',
    'test/benchmark-loop-count.c',
    'test/benchmark-million-async.c',
    'test/benchmark-million-timers.c',
    'test/benchmark-multi-accept.c',
    'test/benchmark-ping-pongs.c',
    'test/benchmark-ping-udp.c',
    'test/benchmark-pound.c',
    'test/benchmark-pump.c',
    'test/benchmark-queue-work.c',
    'test/benchmark-queue-work.c',
    'test/benchmark-sizes.c',
    'test/benchmark-spawn.c',
    'test/benchmark-tcp-write-batch.c',
    'test/benchmark-thread.c',
    'test/benchmark-udp-pummel.c',
    'test/blackhole-server.c',
    'test/echo-server.c',
    'test/run-benchmarks.c',
    'test/runner.c',
  )
  libuv_bench_exe = executable(
    'uv_run_benchmarks',
    libuv_bench_src,
    dependencies: [libuv_dep, libuv_test_deps],
    c_args: libuv_cargs,
  )
  benchmark(
    'uv_run_benchmarks',
    libuv_bench_exe,
    timeout: 6000,
  )
endif

if libuv_build_tests
  libuv_test_src += files(
    'test/blackhole-server.c',
    'test/echo-server.c',
    'test/run-tests.c',
    'test/runner.c',
    'test/test-active.c',
    'test/test-async-null-cb.c',
    'test/test-async.c',
    'test/test-barrier.c',
    'test/test-callback-stack.c',
    'test/test-close-fd.c',
    'test/test-close-order.c',
    'test/test-condvar.c',
    'test/test-connect-unspecified.c',
    'test/test-connection-fail.c',
    'test/test-cwd-and-chdir.c',
    'test/test-default-loop-close.c',
    'test/test-delayed-accept.c',
    'test/test-dlerror.c',
    'test/test-eintr-handling.c',
    'test/test-embed.c',
    'test/test-emfile.c',
    'test/test-env-vars.c',
    'test/test-error.c',
    'test/test-fail-always.c',
    'test/test-fork.c',
    'test/test-fs-copyfile.c',
    'test/test-fs-event.c',
    'test/test-fs-fd-hash.c',
    'test/test-fs-open-flags.c',
    'test/test-fs-poll.c',
    'test/test-fs-readdir.c',
    'test/test-fs.c',
    'test/test-get-currentexe.c',
    'test/test-get-loadavg.c',
    'test/test-get-memory.c',
    'test/test-get-passwd.c',
    'test/test-getaddrinfo.c',
    'test/test-gethostname.c',
    'test/test-getnameinfo.c',
    'test/test-getsockname.c',
    'test/test-getters-setters.c',
    'test/test-gettimeofday.c',
    'test/test-handle-fileno.c',
    'test/test-homedir.c',
    'test/test-hrtime.c',
    'test/test-idle.c',
    'test/test-idna.c',
    'test/test-iouring-pollhup.c',
    'test/test-ip-name.c',
    'test/test-ip4-addr.c',
    'test/test-ip6-addr.c',
    'test/test-ipc-heavy-traffic-deadlock-bug.c',
    'test/test-ipc-send-recv.c',
    'test/test-ipc.c',
    'test/test-loop-alive.c',
    'test/test-loop-close.c',
    'test/test-loop-configure.c',
    'test/test-loop-handles.c',
    'test/test-loop-oom.c',
    'test/test-loop-stop.c',
    'test/test-loop-time.c',
    'test/test-metrics.c',
    'test/test-multiple-listen.c',
    'test/test-mutexes.c',
    'test/test-not-readable-nor-writable-on-read-error.c',
    'test/test-not-writable-after-shutdown.c',
    'test/test-osx-select.c',
    'test/test-pass-always.c',
    'test/test-ping-pong.c',
    'test/test-pipe-bind-error.c',
    'test/test-pipe-close-stdout-read-stdin.c',
    'test/test-pipe-connect-error.c',
    'test/test-pipe-connect-multiple.c',
    'test/test-pipe-connect-prepare.c',
    'test/test-pipe-getsockname.c',
    'test/test-pipe-pending-instances.c',
    'test/test-pipe-sendmsg.c',
    'test/test-pipe-server-close.c',
    'test/test-pipe-set-fchmod.c',
    'test/test-pipe-set-non-blocking.c',
    'test/test-platform-output.c',
    'test/test-poll-close-doesnt-corrupt-stack.c',
    'test/test-poll-close.c',
    'test/test-poll-closesocket.c',
    'test/test-poll-multiple-handles.c',
    'test/test-poll-oob.c',
    'test/test-poll.c',
    'test/test-process-priority.c',
    'test/test-process-title-threadsafe.c',
    'test/test-process-title.c',
    'test/test-queue-foreach-delete.c',
    'test/test-random.c',
    'test/test-readable-on-eof.c',
    'test/test-ref.c',
    'test/test-run-nowait.c',
    'test/test-run-once.c',
    'test/test-semaphore.c',
    'test/test-shutdown-close.c',
    'test/test-shutdown-eof.c',
    'test/test-shutdown-simultaneous.c',
    'test/test-shutdown-twice.c',
    'test/test-signal-multiple-loops.c',
    'test/test-signal-pending-on-close.c',
    'test/test-signal.c',
    'test/test-socket-buffer-size.c',
    'test/test-spawn.c',
    'test/test-stdio-over-pipes.c',
    'test/test-strscpy.c',
    'test/test-strtok.c',
    'test/test-tcp-alloc-cb-fail.c',
    'test/test-tcp-bind-error.c',
    'test/test-tcp-bind6-error.c',
    'test/test-tcp-close-accept.c',
    'test/test-tcp-close-after-read-timeout.c',
    'test/test-tcp-close-reset.c',
    'test/test-tcp-close-while-connecting.c',
    'test/test-tcp-close.c',
    'test/test-tcp-connect-error-after-write.c',
    'test/test-tcp-connect-error.c',
    'test/test-tcp-connect-timeout.c',
    'test/test-tcp-connect6-error.c',
    'test/test-tcp-create-socket-early.c',
    'test/test-tcp-flags.c',
    'test/test-tcp-oob.c',
    'test/test-tcp-open.c',
    'test/test-tcp-read-stop-start.c',
    'test/test-tcp-read-stop.c',
    'test/test-tcp-reuseport.c',
    'test/test-tcp-rst.c',
    'test/test-tcp-shutdown-after-write.c',
    'test/test-tcp-try-write-error.c',
    'test/test-tcp-try-write.c',
    'test/test-tcp-unexpected-read.c',
    'test/test-tcp-write-after-connect.c',
    'test/test-tcp-write-fail.c',
    'test/test-tcp-write-in-a-row.c',
    'test/test-tcp-write-queue-order.c',
    'test/test-tcp-write-to-half-open-connection.c',
    'test/test-tcp-writealot.c',
    'test/test-test-macros.c',
    'test/test-thread-affinity.c',
    'test/test-thread-equal.c',
    'test/test-thread-name.c',
    'test/test-thread-priority.c',
    'test/test-thread.c',
    'test/test-threadpool-cancel.c',
    'test/test-threadpool.c',
    'test/test-timer-again.c',
    'test/test-timer-from-check.c',
    'test/test-timer.c',
    'test/test-tmpdir.c',
    'test/test-tty-duplicate-key.c',
    'test/test-tty-escape-sequence-processing.c',
    'test/test-tty.c',
    'test/test-udp-alloc-cb-fail.c',
    'test/test-udp-bind.c',
    'test/test-udp-connect.c',
    'test/test-udp-connect6.c',
    'test/test-udp-create-socket-early.c',
    'test/test-udp-dgram-too-big.c',
    'test/test-udp-ipv6.c',
    'test/test-udp-mmsg.c',
    'test/test-udp-multicast-interface.c',
    'test/test-udp-multicast-interface6.c',
    'test/test-udp-multicast-join.c',
    'test/test-udp-multicast-join6.c',
    'test/test-udp-multicast-ttl.c',
    'test/test-udp-open.c',
    'test/test-udp-options.c',
    'test/test-udp-recv-in-a-row.c',
    'test/test-udp-reuseport.c',
    'test/test-udp-send-and-recv.c',
    'test/test-udp-send-hang-loop.c',
    'test/test-udp-send-immediate.c',
    'test/test-udp-send-unreachable.c',
    'test/test-udp-sendmmsg-error.c',
    'test/test-udp-try-send.c',
    'test/test-uname.c',
    'test/test-walk-handles.c',
    'test/test-watcher-cross-stop.c',
  )

  libuv_test_cargs = cc.get_supported_arguments(
    '-Wno-int-conversion',
    # TODO: pending https://github.com/libuv/libuv/pull/4673
    '-Wno-incompatible-pointer-types',
    '-D_GNU_SOURCE',
  )

  if cc_msvc
    if get_option('default_library') == 'both'
      error('default_library=both is not supported with MSVC')
    elif get_option('default_library') == 'shared'
      libuv_test_cargs += ['-DUSING_UV_SHARED=1']
    endif
    libuv_test_lflags += [
      '/MANIFEST:EMBED',
      '/MANIFESTINPUT:' + meson.current_source_dir() / 'uv_win_longpath.manifest',
    ]
  endif

  libuv_test_exe = executable(
    'uv_run_tests',
    libuv_test_src,
    dependencies: [libuv_dep, libuv_test_deps],
    c_args: libuv_test_cargs,
    link_args: libuv_test_lflags,
  )
  # in Windows we need to copy the test executable without extension (needed
  # for test-spawn.c in libuv), but that's not possible because run_command()
  # runs before executable() above creates the test executable in build
  # directory, so we create a symlink instead
  if win32
    link_file_in_build_dir = find_program('link_file_in_build_dir.py')
    run_command(
      link_file_in_build_dir,
      'uv_run_tests.exe',
      'uv_run_tests_no_ext',
      check: true,
    )
  endif
  test(
    'libuv_run_tests',
    libuv_test_exe,
    workdir: meson.current_source_dir(),
    protocol: 'tap',
    timeout: 300,
    is_parallel: false,
  )
endif
