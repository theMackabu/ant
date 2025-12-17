import { test, summary } from './helpers.js';

console.log('Console Tests\n');

test('console.log exists', typeof console.log, 'function');
test('console.error exists', typeof console.error, 'function');
test('console.warn exists', typeof console.warn, 'function');
test('console.info exists', typeof console.info, 'function');
test('console.debug exists', typeof console.debug, 'function');
test('console.trace exists', typeof console.trace, 'function');
test('console.time exists', typeof console.time, 'function');
test('console.timeEnd exists', typeof console.timeEnd, 'function');
test('console.assert exists', typeof console.assert, 'function');
test('console.clear exists', typeof console.clear, 'function');

summary();
