const { EventEmitter } = require('events');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const ee = new EventEmitter();
const calls = [];

function onFoo(value) {
  calls.push(['on', value]);
}

function onceFoo(value) {
  calls.push(['once', value]);
}

ee.on('foo', onFoo);
ee.once('foo', onceFoo);

const listenersBefore = ee.listeners('foo');
assert(Array.isArray(listenersBefore), 'listeners() should return an array');
assert(listenersBefore.length === 2, `expected 2 listeners, got ${listenersBefore.length}`);
assert(listenersBefore[0] === onFoo, 'listeners() should return the original persistent listener');
assert(listenersBefore[1] === onceFoo, 'listeners() should unwrap once listeners to the original callback');

const rawBefore = ee.rawListeners('foo');
assert(rawBefore.length === 2, `expected 2 raw listeners, got ${rawBefore.length}`);
assert(rawBefore[0] === onFoo, 'rawListeners() should return the original persistent listener');
assert(rawBefore[1] !== onceFoo, 'rawListeners() should expose the once wrapper');

ee.emit('foo', 1);

const listenersAfter = ee.listeners('foo');
assert(listenersAfter.length === 1, `expected 1 listener after once emission, got ${listenersAfter.length}`);
assert(listenersAfter[0] === onFoo, 'persistent listener should remain after once emission');

assert(calls.length === 2, `expected 2 calls, got ${calls.length}`);
assert(calls[0][0] === 'on' && calls[0][1] === 1, 'persistent listener should fire first');
assert(calls[1][0] === 'once' && calls[1][1] === 1, 'once listener should fire once');

console.log('events listeners api ok');
