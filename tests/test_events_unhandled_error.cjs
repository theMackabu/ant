const assert = require('node:assert');
const { EventEmitter } = require('node:events');

const emitter = new EventEmitter();
const failure = new RangeError('boom');

assert.throws(() => emitter.emit('error', failure), thrown => thrown === failure);
assert.throws(() => new EventEmitter().emit('error', failure), thrown => thrown === failure);

assert.throws(() => emitter.emit('error', 'plain'), thrown => {
  assert.ok(thrown instanceof Error);
  assert.strictEqual(thrown.code, 'ERR_UNHANDLED_ERROR');
  assert.strictEqual(thrown.context, 'plain');
  assert.strictEqual(thrown.message, "Unhandled error. ('plain')");
  return true;
});

assert.throws(() => emitter.emit('error'), { code: 'ERR_UNHANDLED_ERROR' });
assert.strictEqual(emitter.emit('other', failure), false);

const seen = [];
emitter.on('error', error => seen.push(error));
assert.strictEqual(emitter.emit('error', failure), true);
assert.deepStrictEqual(seen, [failure]);

emitter.removeAllListeners('error');
assert.throws(() => emitter.emit('error', failure), thrown => thrown === failure);

console.log('ok');
