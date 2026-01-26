import { test, summary } from './helpers.js';

console.log('Process Tests\n');

test('process exists', typeof process, 'object');
test('process toStringTag', Object.prototype.toString.call(process), '[object process]');

test('pid is number', typeof process.pid, 'number');
test('pid is positive', process.pid > 0, true);

test('platform is string', typeof process.platform, 'string');
test('platform valid', ['darwin', 'linux', 'win32', 'freebsd'].includes(process.platform), true);

test('arch is string', typeof process.arch, 'string');
test('arch valid', ['x64', 'ia32', 'arm64', 'arm'].includes(process.arch), true);

test('argv is array', Array.isArray(process.argv), true);
test('argv has entries', process.argv.length >= 1, true);
test('execArgv is array', Array.isArray(process.execArgv), true);
test('argv0 is string', typeof process.argv0, 'string');
test('execPath is string', typeof process.execPath, 'string');
test('ppid is number', typeof process.ppid, 'number');
test('ppid is positive', process.ppid > 0, true);

test('cwd is function', typeof process.cwd, 'function');
const cwd = process.cwd();
test('cwd returns string', typeof cwd, 'string');
test('cwd not empty', cwd.length > 0, true);

test('env is object', typeof process.env, 'object');
test('env has PATH or Path', process.env.PATH !== undefined || process.env.Path !== undefined, true);

test('exit is function', typeof process.exit, 'function');

test('on is function', typeof process.on, 'function');
test('once is function', typeof process.once, 'function');
test('off is function', typeof process.off, 'function');
test('addListener is function', typeof process.addListener, 'function');
test('removeListener is function', typeof process.removeListener, 'function');
test('removeAllListeners is function', typeof process.removeAllListeners, 'function');
test('emit is function', typeof process.emit, 'function');
test('listenerCount is function', typeof process.listenerCount, 'function');
test('setMaxListeners is function', typeof process.setMaxListeners, 'function');
test('getMaxListeners is function', typeof process.getMaxListeners, 'function');

let onCalled = false;
process.on('testOn', () => {
  onCalled = true;
});
process.emit('testOn');
test('on/emit works', onCalled, true);
process.removeAllListeners('testOn');

let onceCount = 0;
process.once('testOnce', () => {
  onceCount++;
});
process.emit('testOnce');
process.emit('testOnce');
test('once fires once', onceCount, 1);

let offCount = 0;
const offHandler = () => {
  offCount++;
};
process.on('testOff', offHandler);
process.emit('testOff');
process.off('testOff', offHandler);
process.emit('testOff');
test('off removes listener', offCount, 1);

let addListenerCalled = false;
process.addListener('testAddListener', () => {
  addListenerCalled = true;
});
process.emit('testAddListener');
test('addListener works', addListenerCalled, true);
process.removeAllListeners('testAddListener');

let removeListenerCount = 0;
const rlHandler = () => {
  removeListenerCount++;
};
process.on('testRL', rlHandler);
process.emit('testRL');
process.removeListener('testRL', rlHandler);
process.emit('testRL');
test('removeListener works', removeListenerCount, 1);

process.on('testCount', () => {});
process.on('testCount', () => {});
test('listenerCount returns count', process.listenerCount('testCount'), 2);
process.removeAllListeners('testCount');
test('removeAllListeners clears', process.listenerCount('testCount'), 0);

let receivedArg = null;
process.on('testArgs', arg => {
  receivedArg = arg;
});
process.emit('testArgs', 'hello');
test('emit passes arguments', receivedArg, 'hello');
process.removeAllListeners('testArgs');

const chainResult = process.on('testChain', () => {});
test('on returns process', chainResult, process);
process.removeAllListeners('testChain');

test('getMaxListeners default', process.getMaxListeners(), 10);

process.setMaxListeners(20);
test('setMaxListeners updates', process.getMaxListeners(), 20);
process.setMaxListeners(10);

process.setMaxListeners(0);
for (let i = 0; i < 15; i++) {
  process.on('manyListeners', () => {});
}
test('supports many listeners', process.listenerCount('manyListeners'), 15);
process.removeAllListeners('manyListeners');
process.setMaxListeners(10);

test('stdin is object', typeof process.stdin, 'object');
test('stdin.isTTY is boolean', typeof process.stdin.isTTY, 'boolean');
test('stdin.setRawMode is function', typeof process.stdin.setRawMode, 'function');
test('stdin.resume is function', typeof process.stdin.resume, 'function');
test('stdin.pause is function', typeof process.stdin.pause, 'function');
test('stdin.on is function', typeof process.stdin.on, 'function');
test('stdin.removeAllListeners is function', typeof process.stdin.removeAllListeners, 'function');

test('stdout is object', typeof process.stdout, 'object');
test('stdout.isTTY is boolean', typeof process.stdout.isTTY, 'boolean');
test('stdout.rows is number', typeof process.stdout.rows, 'number');
test('stdout.columns is number', typeof process.stdout.columns, 'number');
test('stdout.write is function', typeof process.stdout.write, 'function');
test('stdout.on is function', typeof process.stdout.on, 'function');
test('stdout.once is function', typeof process.stdout.once, 'function');
test('stdout.removeAllListeners is function', typeof process.stdout.removeAllListeners, 'function');
test('stdout.getWindowSize is function', typeof process.stdout.getWindowSize, 'function');

const windowSize = process.stdout.getWindowSize();
test('getWindowSize returns array', Array.isArray(windowSize), true);
test('getWindowSize has 2 elements', windowSize.length, 2);
test('getWindowSize cols is number', typeof windowSize[0], 'number');
test('getWindowSize rows is number', typeof windowSize[1], 'number');

test('stderr is object', typeof process.stderr, 'object');
test('stderr.isTTY is boolean', typeof process.stderr.isTTY, 'boolean');
test('stderr.write is function', typeof process.stderr.write, 'function');
test('stderr.on is function', typeof process.stderr.on, 'function');
test('stderr.once is function', typeof process.stderr.once, 'function');
test('stderr.removeAllListeners is function', typeof process.stderr.removeAllListeners, 'function');

test('version is string', typeof process.version, 'string');
test('version starts with v', process.version.startsWith('v'), true);

test('versions is object', typeof process.versions, 'object');
test('versions.ant exists', typeof process.versions.ant, 'string');
test('versions.uv exists', typeof process.versions.uv, 'string');

test('release is object', typeof process.release, 'object');
test('release.name is string', typeof process.release.name, 'string');

test('uptime is function', typeof process.uptime, 'function');
const uptime = process.uptime();
test('uptime returns number', typeof uptime, 'number');
test('uptime is non-negative', uptime >= 0, true);

test('memoryUsage is function', typeof process.memoryUsage, 'function');
const mem = process.memoryUsage();
test('memoryUsage returns object', typeof mem, 'object');
test('memoryUsage.rss is number', typeof mem.rss, 'number');
test('memoryUsage.heapTotal is number', typeof mem.heapTotal, 'number');
test('memoryUsage.heapUsed is number', typeof mem.heapUsed, 'number');
test('memoryUsage.rss method exists', typeof process.memoryUsage.rss, 'function');
test('memoryUsage.rss returns number', typeof process.memoryUsage.rss(), 'number');

test('cpuUsage is function', typeof process.cpuUsage, 'function');
const cpu = process.cpuUsage();
test('cpuUsage returns object', typeof cpu, 'object');
test('cpuUsage.user is number', typeof cpu.user, 'number');
test('cpuUsage.system is number', typeof cpu.system, 'number');

test('hrtime is function', typeof process.hrtime, 'function');
const hr = process.hrtime();
test('hrtime returns array', Array.isArray(hr), true);
test('hrtime has 2 elements', hr.length, 2);
test('hrtime[0] is number', typeof hr[0], 'number');
test('hrtime[1] is number', typeof hr[1], 'number');

test('hrtime.bigint is function', typeof process.hrtime.bigint, 'function');
const hrBigint = process.hrtime.bigint();
test('hrtime.bigint returns bigint', typeof hrBigint, 'bigint');

test('kill is function', typeof process.kill, 'function');
test('abort is function', typeof process.abort, 'function');
test('chdir is function', typeof process.chdir, 'function');
test('umask is function', typeof process.umask, 'function');

const oldMask = process.umask();
test('umask returns number', typeof oldMask, 'number');

if (process.platform !== 'win32') {
  test('getuid is function', typeof process.getuid, 'function');
  test('geteuid is function', typeof process.geteuid, 'function');
  test('getgid is function', typeof process.getgid, 'function');
  test('getegid is function', typeof process.getegid, 'function');
  test('getgroups is function', typeof process.getgroups, 'function');
  test('setuid is function', typeof process.setuid, 'function');
  test('setgid is function', typeof process.setgid, 'function');
  test('seteuid is function', typeof process.seteuid, 'function');
  test('setegid is function', typeof process.setegid, 'function');
  test('setgroups is function', typeof process.setgroups, 'function');
  test('initgroups is function', typeof process.initgroups, 'function');
  
  test('getuid returns number', typeof process.getuid(), 'number');
  test('getgid returns number', typeof process.getgid(), 'number');
  test('getgroups returns array', Array.isArray(process.getgroups()), true);
}

summary();
