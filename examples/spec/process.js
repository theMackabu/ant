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

test('cwd is function', typeof process.cwd, 'function');
const cwd = process.cwd();
test('cwd returns string', typeof cwd, 'string');
test('cwd not empty', cwd.length > 0, true);

test('env is object', typeof process.env, 'object');
test('env has PATH or Path', process.env.PATH !== undefined || process.env.Path !== undefined, true);

test('exit is function', typeof process.exit, 'function');

summary();
