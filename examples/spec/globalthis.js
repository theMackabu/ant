import { test, summary } from './helpers.js';

console.log('GlobalThis Tests\n');

test('globalThis exists', typeof globalThis, 'object');
test('globalThis.console', typeof globalThis.console, 'object');
test('globalThis.setTimeout', typeof globalThis.setTimeout, 'function');
test('globalThis.Object', globalThis.Object === Object, true);
test('globalThis.Array', globalThis.Array === Array, true);
test('globalThis.Math', globalThis.Math === Math, true);
test('globalThis.JSON', globalThis.JSON === JSON, true);

globalThis.testGlobal = 42;
test('set on globalThis', testGlobal, 42);

test('process exists', typeof process, 'object');
test('process.env exists', typeof process.env, 'object');
test('process.argv exists', typeof process.argv, 'object');
test('process.exit exists', typeof process.exit, 'function');
test('process.cwd exists', typeof process.cwd, 'function');

summary();
