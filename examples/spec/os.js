import { test, summary } from './helpers.js';
import os from 'ant:os';

console.log('OS Module Tests\n');

test('os.EOL exists', typeof os.EOL, 'string');
test('os.EOL is newline', os.EOL === '\n' || os.EOL === '\r\n', true);

test('os.devNull exists', typeof os.devNull, 'string');
test('os.devNull value', os.devNull === '/dev/null' || os.devNull === '\\\\.\\nul', true);

test('os.arch returns string', typeof os.arch(), 'string');
test('os.arch valid value', ['x64', 'arm64', 'arm', 'ia32', 'ppc64', 's390x', 'mips64', 'mipsel', 'loong64', 'riscv64'].includes(os.arch()), true);

test('os.platform returns string', typeof os.platform(), 'string');
test('os.platform valid value', ['darwin', 'linux', 'win32', 'freebsd', 'openbsd', 'sunos', 'aix'].includes(os.platform()), true);

test('os.type returns string', typeof os.type(), 'string');
test('os.type valid value', ['Darwin', 'Linux', 'Windows_NT', 'FreeBSD', 'OpenBSD'].includes(os.type()), true);

test('os.release returns string', typeof os.release(), 'string');
test('os.release not empty', os.release().length > 0, true);

test('os.version returns string', typeof os.version(), 'string');

test('os.machine returns string', typeof os.machine(), 'string');
test('os.machine not empty', os.machine().length > 0, true);

test('os.hostname returns string', typeof os.hostname(), 'string');
test('os.hostname not empty', os.hostname().length > 0, true);

test('os.homedir returns string', typeof os.homedir(), 'string');
test('os.homedir not empty', os.homedir().length > 0, true);

test('os.tmpdir returns string', typeof os.tmpdir(), 'string');
test('os.tmpdir not empty', os.tmpdir().length > 0, true);

test('os.endianness returns string', typeof os.endianness(), 'string');
test('os.endianness valid value', os.endianness() === 'LE' || os.endianness() === 'BE', true);

test('os.uptime returns number', typeof os.uptime(), 'number');
test('os.uptime is positive', os.uptime() > 0, true);

test('os.totalmem returns number', typeof os.totalmem(), 'number');
test('os.totalmem is positive', os.totalmem() > 0, true);

test('os.freemem returns number', typeof os.freemem(), 'number');
test('os.freemem is positive', os.freemem() > 0, true);
test('os.freemem <= totalmem', os.freemem() <= os.totalmem(), true);

test('os.availableParallelism returns number', typeof os.availableParallelism(), 'number');
test('os.availableParallelism >= 1', os.availableParallelism() >= 1, true);

const cpus = os.cpus();
test('os.cpus returns array', Array.isArray(cpus), true);
test('os.cpus has entries', cpus.length > 0, true);
if (cpus.length > 0) {
  test('os.cpus[0] has model', typeof cpus[0].model, 'string');
  test('os.cpus[0] has speed', typeof cpus[0].speed, 'number');
  test('os.cpus[0] has times', typeof cpus[0].times, 'object');
  test('os.cpus[0].times has user', typeof cpus[0].times.user, 'number');
  test('os.cpus[0].times has sys', typeof cpus[0].times.sys, 'number');
  test('os.cpus[0].times has idle', typeof cpus[0].times.idle, 'number');
}

const loadavg = os.loadavg();
test('os.loadavg returns array', Array.isArray(loadavg), true);
test('os.loadavg has 3 entries', loadavg.length, 3);
test('os.loadavg[0] is number', typeof loadavg[0], 'number');
test('os.loadavg[1] is number', typeof loadavg[1], 'number');
test('os.loadavg[2] is number', typeof loadavg[2], 'number');

const netInterfaces = os.networkInterfaces();
test('os.networkInterfaces returns object', typeof netInterfaces, 'object');

const userInfo = os.userInfo();
test('os.userInfo returns object', typeof userInfo, 'object');
test('os.userInfo has uid', typeof userInfo.uid, 'number');
test('os.userInfo has gid', typeof userInfo.gid, 'number');
test('os.userInfo has username', typeof userInfo.username, 'string');
test('os.userInfo has homedir', typeof userInfo.homedir, 'string');

test('os.getPriority returns number', typeof os.getPriority(), 'number');
test('os.getPriority(0) returns number', typeof os.getPriority(0), 'number');

test('os.constants exists', typeof os.constants, 'object');
test('os.constants.signals exists', typeof os.constants.signals, 'object');
test('os.constants.signals.SIGINT exists', typeof os.constants.signals.SIGINT, 'number');
test('os.constants.signals.SIGTERM exists', typeof os.constants.signals.SIGTERM, 'number');
test('os.constants.errno exists', typeof os.constants.errno, 'object');
test('os.constants.errno.ENOENT exists', typeof os.constants.errno.ENOENT, 'number');
test('os.constants.priority exists', typeof os.constants.priority, 'object');
test('os.constants.priority.PRIORITY_NORMAL', os.constants.priority.PRIORITY_NORMAL, 0);

test('os.constants.dlopen exists', typeof os.constants.dlopen, 'object');
test('os.constants.dlopen.RTLD_LAZY exists', typeof os.constants.dlopen.RTLD_LAZY, 'number');
test('os.constants.dlopen.RTLD_NOW exists', typeof os.constants.dlopen.RTLD_NOW, 'number');
test('os.constants.dlopen.RTLD_GLOBAL exists', typeof os.constants.dlopen.RTLD_GLOBAL, 'number');
test('os.constants.dlopen.RTLD_LOCAL exists', typeof os.constants.dlopen.RTLD_LOCAL, 'number');

test('os.constants.UV_UDP_REUSEADDR exists', typeof os.constants.UV_UDP_REUSEADDR, 'number');
test('os.constants.UV_UDP_REUSEADDR value', os.constants.UV_UDP_REUSEADDR, 4);

summary();
