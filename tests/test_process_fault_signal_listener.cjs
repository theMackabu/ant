const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const libcName = process.platform === 'darwin' ? 'libSystem.dylib' : 'libc.so.6';

const scenario = process.argv[2] === 'child' ? process.argv[3] : null;

if (scenario !== null) {
  const { dlopen, FFIType } = require('ant:ffi');
  const libc = dlopen(libcName);
  const strlen = libc.define('strlen', { args: [FFIType.pointer], returns: FFIType.uint64 });
  const mmap = libc.define('mmap', {
    args: [FFIType.pointer, FFIType.uint64, FFIType.int, FFIType.int, FFIType.int, FFIType.int64],
    returns: FFIType.pointer,
  });
  if (process.platform === 'darwin') {
    const taskSelf = libc.define('task_self_trap', { args: [], returns: FFIType.int });
    const setPorts = libc.define('task_set_exception_ports', {
      args: [FFIType.int, FFIType.int, FFIType.int, FFIType.int, FFIType.int],
      returns: FFIType.int,
    });
    setPorts(taskSelf(), 7166, 0, 1, process.arch === 'arm64' ? 5 : 13);
  }
  const listener = name => console.log('listener', name);
  const nullFault = () => { strlen(null); console.log('resumed'); };
  const protectedFault = () => {
    const page = mmap(null, 16384, 0, process.platform === 'darwin' ? 0x1002 : 0x22, -1, 0);
    strlen(page);
    console.log('resumed');
  };
  new Function('listener', 'nullFault', 'protectedFault', scenario)(listener, nullFault, protectedFault);
  return;
}

const run = body => {
  const env = { ...process.env, NO_COLOR: '1', DO_NOT_TRACK: '1' };
  delete env.FORCE_COLOR;
  const child = spawnSync(process.execPath, [__filename, 'child', body], {
    encoding: 'utf8',
    timeout: 20000,
    killSignal: 'SIGKILL',
    env,
  });
  return {
    timedOut: !!child.error || child.signal === 'SIGKILL',
    status: child.status,
    signal: child.signal,
    stdout: child.stdout,
    reported: (/\((SIG[A-Z]+)\)/.exec(child.stderr || '') || [])[1],
  };
};

const probe = process.platform === 'win32' ? { status: 1 } : run("console.log('loaded')");

if (probe.status !== 0 || probe.stdout !== 'loaded\n') {
  console.log(`process fault signal listener skipped: cannot fault through ${libcName}`);
} else {
  const crashed = (result, expected, label) => {
    assert.strictEqual(result.timedOut, false, `${label}: timed out`);
    assert.strictEqual(result.stdout, expected.stdout, label);
    assert.strictEqual(result.signal, expected.signal, label);
    assert.strictEqual(result.status, expected.status, label);
    assert.strictEqual(result.reported, expected.reported, label);
  };

  const plain = run('nullFault();');
  assert.strictEqual(plain.timedOut, false, 'no listener: timed out');
  assert.strictEqual(plain.stdout, '');
  assert.strictEqual(plain.status, null);
  assert.strictEqual(plain.signal, 'SIGTRAP');
  assert.strictEqual(plain.reported, 'SIGSEGV');

  crashed(run("process.on('SIGSEGV', listener); nullFault();"), plain, 'listener');
  crashed(run("process.once('SIGSEGV', listener); nullFault();"), plain, 'once listener');
  crashed(
    run("process.on('SIGSEGV', listener); process.on('SIGSEGV', () => {}); nullFault();"),
    plain,
    'two listeners'
  );
  crashed(
    run("process.on('SIGSEGV', listener); process.off('SIGSEGV', listener); nullFault();"),
    plain,
    'removed listener'
  );
  crashed(
    run("process.on('SIGSEGV', listener); process.removeAllListeners('SIGSEGV'); nullFault();"),
    plain,
    'removed all listeners'
  );

  const plainProtected = run('protectedFault();');
  assert.strictEqual(plainProtected.timedOut, false, 'protected page, no listener: timed out');
  assert.strictEqual(plainProtected.stdout, '');
  assert.strictEqual(plainProtected.signal, 'SIGTRAP');
  assert.match(plainProtected.reported, /^SIG(SEGV|BUS)$/);
  crashed(
    run("process.on('SIGSEGV', listener); process.on('SIGBUS', listener); protectedFault();"),
    plainProtected,
    'protected page with listeners'
  );

  for (const name of ['SIGSEGV', 'SIGBUS', 'SIGFPE', 'SIGILL']) {
    const sent = run(`
      process.on('${name}', listener);
      process.kill(process.pid, '${name}');
      process.kill(process.pid, '${name}');
      console.log('sent');
      setTimeout(() => {
        process.kill(process.pid, '${name}');
        setTimeout(() => console.log('done'), 50);
      }, 50);
    `);
    assert.deepStrictEqual(
      sent,
      {
        timedOut: false,
        status: 0,
        signal: null,
        stdout: `sent\nlistener ${name}\nlistener ${name}\nlistener ${name}\ndone\n`,
        reported: undefined,
      },
      `sent ${name}`
    );
  }

  const sentThenFault = run(`
    process.once('SIGSEGV', listener);
    process.kill(process.pid, 'SIGSEGV');
    setTimeout(nullFault, 50);
  `);
  crashed(sentThenFault, { ...plain, stdout: 'listener SIGSEGV\n' }, 'fault after a consumed once listener');

  const sentWhileListening = run(`
    process.on('SIGSEGV', listener);
    process.kill(process.pid, 'SIGSEGV');
    setTimeout(nullFault, 50);
  `);
  crashed(sentWhileListening, { ...plain, stdout: 'listener SIGSEGV\n' }, 'fault after a delivered signal');

  const sentWithout = run("process.kill(process.pid, 'SIGSEGV'); setTimeout(() => console.log('alive'), 200);");
  crashed(sentWithout, plain, 'sent without listener');

  const aborted = run("process.on('SIGABRT', listener); process.abort(); console.log('resumed');");
  assert.strictEqual(aborted.timedOut, false, 'abort with listener: timed out');
  assert.strictEqual(aborted.stdout, '');
  assert.strictEqual(aborted.signal, 'SIGABRT');

  console.log('process fault signal listener ok');
}
