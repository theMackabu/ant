// process.exitCode sets the status of a natural exit, and process.on('exit')
// runs once with the final code on natural exit, process.exit() and fatal
// uncaught errors. Listeners may change exitCode or call process.exit again.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const run = code => {
  const env = { ...process.env, NO_COLOR: '1' };
  delete env.FORCE_COLOR;
  const child = spawnSync(process.execPath, ['-e', code], { encoding: 'utf8', timeout: 30000, env });
  return [child.status, child.stdout];
};

const cases = [
  ["process.on('exit', c => console.log('exit', c, process.exitCode)); console.log('main')", 0, 'main\nexit 0 undefined\n'],
  ["process.exitCode = 4; process.on('exit', c => console.log('exit', c))", 4, 'exit 4\n'],
  ["process.on('exit', c => { console.log('exit', c); process.exitCode = 9 })", 9, 'exit 0\n'],
  ["process.on('exit', c => console.log('exit', c)); process.exit(3); console.log('nope')", 3, 'exit 3\n'],
  ["process.exitCode = 5; process.on('exit', c => console.log('exit', c)); process.exit()", 5, 'exit 5\n'],
  ["process.on('exit', c => { console.log('exit', c); process.exit(6); console.log('after') }); process.on('exit', () => console.log('second'))", 6, 'exit 0\n'],
  ["process.on('exit', c => console.log('exit', c)); throw new Error('x')", 1, 'exit 1\n'],
  ["process.exitCode = 3; setTimeout(() => { throw new Error('x') })", 1, ''],
  ["process.on('exit', () => { setTimeout(() => console.log('timer')); console.log('exit') })", 0, 'exit\n'],
  ["process.on('beforeExit', c => console.log('beforeExit', c)); process.exitCode = 2", 2, 'beforeExit 2\n'],
  ["process.on('exit', () => { throw new Error('in exit') })", 1, ''],
  ["process.exitCode = 0; process.on('exit', () => { throw new Error('in exit') })", 0, ''],
  ["process.on('exit', c => console.log('exit', c)); process.on('uncaughtException', () => console.log('caught')); throw 1", 0, 'caught\nexit 0\n'],
  ["process.on('exit', function (c) { console.log(this === process, arguments.length, process._exiting) }); console.log(process._exiting); process.exit(0)", 0, 'false\ntrue 1 true\n'],
  ["setTimeout(() => process.exit(2)); process.on('exit', c => console.log('exit', c, process.exitCode))", 2, 'exit 2 2\n'],
  ["process.exitCode = -1", 255, ''],
  ["process.exitCode = 256", 0, ''],
  ["process.exit('7')", 7, ''],
];

for (const [code, status, stdout] of cases) assert.deepStrictEqual(run(code), [status, stdout], code);

const values = [undefined, null, 3, '7', 2.5, 'x', '', true, 10n, -1, 2 ** 31, NaN, 2 ** 53, '1.5', { valueOf: () => 1 }];
const seen = values.map(v => {
  try { process.exitCode = v; return process.exitCode } catch (e) { return `${e.name} ${e.code}` }
});
process.exitCode = undefined;

assert.deepStrictEqual(seen, [
  undefined, undefined, 3, 7, 'RangeError ERR_OUT_OF_RANGE', 'TypeError ERR_INVALID_ARG_TYPE',
  'TypeError ERR_INVALID_ARG_TYPE', 'TypeError ERR_INVALID_ARG_TYPE', 'TypeError ERR_INVALID_ARG_TYPE',
  -1, -2147483648, 'RangeError ERR_OUT_OF_RANGE', 'RangeError ERR_OUT_OF_RANGE', 'RangeError ERR_OUT_OF_RANGE',
  'TypeError ERR_INVALID_ARG_TYPE',
]);
assert.throws(() => process.exit('x'), { code: 'ERR_INVALID_ARG_TYPE' });

const desc = Object.getOwnPropertyDescriptor(process, 'exitCode');
assert.deepStrictEqual([typeof desc.get, typeof desc.set, desc.enumerable, desc.configurable], ['function', 'function', true, false]);

console.log('PASS process exit code and exit event match Node');
