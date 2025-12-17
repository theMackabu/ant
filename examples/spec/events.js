import { test, summary } from './helpers.js';
import { EventEmitter } from 'events';

console.log('EventEmitter Tests\n');

const ee = new EventEmitter();

let received = null;
ee.on('test', (data) => {
  received = data;
});
ee.emit('test', 'hello');
test('on and emit', received, 'hello');

let count = 0;
ee.on('count', () => count++);
ee.on('count', () => count++);
ee.emit('count');
test('multiple listeners', count, 2);

let onceValue = 0;
ee.once('once', (val) => {
  onceValue = val;
});
ee.emit('once', 42);
ee.emit('once', 100);
test('once only fires once', onceValue, 42);

let removed = false;
const handler = () => { removed = true; };
ee.on('remove', handler);
ee.off('remove', handler);
ee.emit('remove');
test('off removes listener', removed, false);

let allRemoved = 0;
ee.on('all', () => allRemoved++);
ee.on('all', () => allRemoved++);
ee.removeAllListeners('all');
ee.emit('all');
test('removeAllListeners', allRemoved, 0);

test('listenerCount', ee.listenerCount('count'), 2);

const names = ee.eventNames();
test('eventNames includes count', names.includes('count'), true);

summary();
