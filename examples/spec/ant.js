import { test, summary } from './helpers.js';

console.log('Ant Global Tests\n');

test('Ant exists', typeof Ant, 'object');
test('version is string', typeof Ant.version, 'string');
test('version format', /^\d+\.\d+\.\d+/.test(Ant.version), true);
test('target is string', typeof Ant.target, 'string');
test('target not empty', Ant.target.length > 0, true);
test('revision is string', typeof Ant.revision, 'string');
test('buildDate is string', typeof Ant.buildDate, 'string');
test('host is string', typeof Ant.host, 'string');

const validHosts = [
  'cygwin',
  'darwin',
  'dragonfly',
  'emscripten',
  'freebsd',
  'gnu',
  'haiku',
  'linux',
  'netbsd',
  'openbsd',
  'windows',
  'sunos',
  'os/2'
];

test('host is valid', validHosts.includes(Ant.host), true);

test('typeof is function', typeof Ant.typeof, 'function');
test('typeof number', Ant.typeof(42), 'number');
test('typeof string', Ant.typeof('hello'), 'string');
test('typeof object', Ant.typeof({}), 'object');
test('typeof array', Ant.typeof([]), 'array');

test(
  'typeof function',
  Ant.typeof(() => {}),
  'function'
);

test('typeof null', Ant.typeof(null), 'null');
test('typeof undefined', Ant.typeof(undefined), 'undefined');
test('typeof boolean', Ant.typeof(true), 'boolean');

test('gc is function', typeof Ant.gc, 'function');
const gcResult = Ant.gc();
test('gc returns object', typeof gcResult, 'object');
test('gc has heapBefore', typeof gcResult.heapBefore, 'number');
test('gc has heapAfter', typeof gcResult.heapAfter, 'number');
test('gc has freed', typeof gcResult.freed, 'number');

test('alloc is function', typeof Ant.alloc, 'function');
const allocResult = Ant.alloc();
test('alloc returns object', typeof allocResult, 'object');
test('alloc has arenaSize', typeof allocResult.arenaSize, 'number');
test('alloc has heapSize', typeof allocResult.heapSize, 'number');
test('alloc has freeBytes', typeof allocResult.freeBytes, 'number');

test('stats is function', typeof Ant.stats, 'function');
test('raw is object', typeof Ant.raw, 'object');
test('raw.typeof is function', typeof Ant.raw.typeof, 'function');
test('serve is function', typeof Ant.serve, 'function');
test('sleep is function', typeof Ant.sleep, 'function');
test('msleep is function', typeof Ant.msleep, 'function');
test('usleep is function', typeof Ant.usleep, 'function');
test('signal is function', typeof Ant.signal, 'function');

summary();
