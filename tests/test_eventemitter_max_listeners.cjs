const assert = require('node:assert');
const events = require('node:events');
const { EventEmitter } = events;
const { PassThrough } = require('node:stream');

const ee = new EventEmitter();
assert.equal(typeof ee.once, 'function');
assert.equal(typeof ee.prependListener, 'function');
assert.equal(typeof ee.prependOnceListener, 'function');
assert.equal(typeof ee.listenerCount, 'function');
assert.equal(typeof ee.setMaxListeners, 'function');
assert.equal(typeof ee.getMaxListeners, 'function');
assert.equal(typeof ee.rawListeners, 'function');

assert.equal(ee.getMaxListeners(), 10);
assert.equal(ee.setMaxListeners(3), ee);
assert.equal(ee.getMaxListeners(), 3);

function a() {}
function b() {}
ee.on('x', a);
ee.prependOnceListener('x', b);

assert.equal(ee.listenerCount('x'), 2);
const raw = ee.rawListeners('x');
assert.equal(raw.length, 2);
assert.notEqual(raw[0], b);
assert.equal(raw[0].listener, b);
assert.equal(raw[1], a);

const pt = new PassThrough();
assert.equal(typeof pt.setMaxListeners, 'function');
assert.equal(typeof pt.getMaxListeners, 'function');
assert.equal(typeof pt.once, 'function');
assert.equal(typeof pt.prependListener, 'function');
assert.equal(typeof pt.prependOnceListener, 'function');
assert.equal(typeof pt.listenerCount, 'function');
assert.equal(typeof pt.rawListeners, 'function');

pt.setMaxListeners(30);
assert.equal(pt.getMaxListeners(), 30);
assert.equal(events.getMaxListeners(pt), 30);
