// An event type caches the key string it was last matched by, so emits with
// that same string skip comparing bytes. Equal strings used in turn must keep
// finding the same listeners, and the cache must not be rewritten on every emit.
const assert = require('node:assert');
const { EventEmitter } = require('node:events');

const e = new EventEmitter();
let total = 0;
e.on('data', v => { total += v; });
const built = ['da', 'ta'].join('');
for (let i = 0; i < 1000; i++) e.emit(i & 1 ? built : 'data', 1);
assert.strictEqual(total, 1000);
assert.strictEqual(e.listenerCount(built), 1);
assert.deepStrictEqual(e.eventNames(), ['data']);
assert.strictEqual(e.emit(['mis', 'sing'].join('')), false);

const sym = Symbol('data');
e.on(sym, () => { total += 100; });
e.emit(sym);
assert.strictEqual(total, 1100);

e.off(built, e.listeners('data')[0]);
assert.strictEqual(e.emit('data', 1), false);
assert.deepStrictEqual(e.eventNames(), [sym]);

const target = new EventTarget();
let pings = 0;
target.addEventListener('ping', () => pings++);
for (let i = 0; i < 10; i++) target.dispatchEvent(new Event(i & 1 ? ['pi', 'ng'].join('') : 'ping'));
assert.strictEqual(pings, 10);

console.log('PASS equal event names find the same listeners');
