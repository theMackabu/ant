const assert = require('node:assert');
const { EventEmitter } = require('node:events');

const emitter = new EventEmitter();
emitter.on('anchor', () => {});

for (let round = 0; round < 40; round++) {
  assert.strictEqual(emitter.emit('data', round), false);
  assert.strictEqual(emitter.emit('dr' + 'ain'.slice(0, 3), round), false);
  assert.throws(() => emitter.emit('error', new Error('unheard')), /unheard/);
  assert.throws(() => emitter.emit(['er', 'ror'].join(''), new Error('joined')), /joined/);
}

let seen = 0;
const listener = () => { seen++; };
emitter.on('data', listener);
assert.strictEqual(emitter.emit('data'), true);
assert.strictEqual(emitter.emit(['da', 'ta'].join('')), true);
emitter.off('data', listener);
assert.strictEqual(emitter.emit('data'), false);
assert.strictEqual(seen, 2);

emitter.on('error', listener);
assert.strictEqual(emitter.emit('error', new Error('heard')), true);
emitter.off('error', listener);
assert.throws(() => emitter.emit('error', new Error('again')), /again/);

const names = [];
for (let i = 0; i < 64; i++) names.push('event-' + i);
const churn = new EventEmitter();
churn.on('anchor', () => {});
let garbage = [];
for (let i = 0; i < 20000; i++) {
  const name = names[i & 63] + '';
  assert.strictEqual(churn.emit(name.slice(0, 5) + name.slice(5), i), false);
  garbage.push({ pad: 'x'.repeat(64) + i });
  if (garbage.length > 2000) garbage = [];
}
churn.on('event-7', listener);
assert.strictEqual(churn.emit('event-' + 7), true);
assert.strictEqual(churn.emit('event-' + 8), false);

const bare = new EventEmitter();
for (let i = 0; i < 100; i++) assert.strictEqual(bare.emit('tick'), false);
assert.throws(() => bare.emit('error', new Error('bare')), /bare/);
bare.on('tick', listener);
assert.strictEqual(bare.emit('tick'), true);

console.log('ok');
