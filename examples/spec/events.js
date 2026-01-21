import { test, summary } from './helpers.js';
import { EventEmitter } from 'ant:events';

console.log('EventEmitter Tests\n');

const ee = new EventEmitter();

let received = null;
ee.on('test', data => {
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
ee.once('once', val => {
  onceValue = val;
});
ee.emit('once', 42);
ee.emit('once', 100);
test('once only fires once', onceValue, 42);

let removed = false;
const handler = () => {
  removed = true;
};
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

const ee2 = new EventEmitter();
let args = [];
ee2.on('multi', (a, b, c) => {
  args = [a, b, c];
});
ee2.emit('multi', 1, 2, 3);
test('multiple emit args', args.join(','), '1,2,3');

const ee3 = new EventEmitter();
test('emit returns false with no listeners', ee3.emit('none'), false);
ee3.on('exists', () => {});
test('emit returns true with listeners', ee3.emit('exists'), true);

const ee4 = new EventEmitter();
const chain = ee4
  .on('a', () => {})
  .once('b', () => {})
  .off('b', () => {});
test('methods return this for chaining', chain, ee4);

const ee5 = new EventEmitter();
let aliasCount = 0;
ee5.addListener('alias', () => aliasCount++);
ee5.emit('alias');
test('addListener alias works', aliasCount, 1);

const ee6 = new EventEmitter();
let removeAliasVal = 0;
const removeAliasHandler = () => {
  removeAliasVal = 1;
};
ee6.on('rem', removeAliasHandler);
ee6.removeListener('rem', removeAliasHandler);
ee6.emit('rem');
test('removeListener alias works', removeAliasVal, 0);

const ee7 = new EventEmitter();
let onceCount = 0;
ee7.once('multi-once', () => onceCount++);
ee7.once('multi-once', () => onceCount++);
ee7.emit('multi-once');
test('multiple once listeners fire', onceCount, 2);
ee7.emit('multi-once');
test('multiple once listeners only fire once', onceCount, 2);

const ee8 = new EventEmitter();
test('listenerCount for non-existent event', ee8.listenerCount('nope'), 0);

const ee9 = new EventEmitter();
const h = () => {};
ee9.on('temp', h);
ee9.off('temp', h);
test('eventNames excludes removed events', ee9.eventNames().includes('temp'), false);

const eeA = new EventEmitter();
const eeB = new EventEmitter();
let aVal = 0,
  bVal = 0;
eeA.on('x', () => aVal++);
eeB.on('x', () => bVal++);
eeA.emit('x');
test('separate instances are isolated (A)', aVal, 1);
test('separate instances are isolated (B)', bVal, 0);

console.log('\nEventTarget Tests\n');

const et = new EventTarget();
let etReceived = null;
et.addEventListener('click', e => {
  etReceived = e.type;
});
et.dispatchEvent('click');
test('EventTarget addEventListener and dispatchEvent', etReceived, 'click');

let etEvent = null;
const et2 = new EventTarget();
et2.addEventListener('custom', e => {
  etEvent = e;
});
et2.dispatchEvent('custom', { foo: 'bar' });
test('event.type', etEvent.type, 'custom');
test('event.target is EventTarget', etEvent.target, et2);
test('event.detail', etEvent.detail.foo, 'bar');

const et3 = new EventTarget();
let et3Val = 0;
const et3Handler = () => {
  et3Val++;
};
et3.addEventListener('rem', et3Handler);
et3.removeEventListener('rem', et3Handler);
et3.dispatchEvent('rem');
test('EventTarget removeEventListener', et3Val, 0);

const et4 = new EventTarget();
let et4Count = 0;
et4.addEventListener('once', () => et4Count++, { once: true });
et4.dispatchEvent('once');
et4.dispatchEvent('once');
test('EventTarget once option', et4Count, 1);

console.log('\nGlobal Event Tests\n');

let globalReceived = null;
addEventListener('global-test', e => {
  globalReceived = e.type;
});
dispatchEvent('global-test');
test('global addEventListener and dispatchEvent', globalReceived, 'global-test');

let globalRemoved = 0;
const globalHandler = () => {
  globalRemoved++;
};
addEventListener('global-rem', globalHandler);
removeEventListener('global-rem', globalHandler);
dispatchEvent('global-rem');
test('global removeEventListener', globalRemoved, 0);

summary();
