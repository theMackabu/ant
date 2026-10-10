const assert = require('node:assert');
const events = require('node:events');

const { EventEmitter } = events;

// Matches Node: an object made from EventEmitter.prototype without the
// constructor is still an emitter (its methods create _events lazily), so
// once() waits for the event; one made from EventTarget.prototype has no
// internal listener state and rejects.
(async () => {
  const timeout = setTimeout(() => {
    throw new Error('events.once prototype spoof timed out');
  }, 1000);

  try {
    const emitter = Object.create(EventEmitter.prototype);
    const ready = events.once(emitter, 'ready');
    emitter.emit('ready', 1, 2);
    assert.deepStrictEqual(await ready, [1, 2]);
    assert.strictEqual(emitter.listenerCount('ready'), 0);

    await assert.rejects(
      events.once(Object.create(EventTarget.prototype), 'ready'),
      TypeError
    );
  } finally {
    clearTimeout(timeout);
  }

  console.log('node-events-once-prototype-spoof:ok');
})().catch((err) => {
  setTimeout(() => {
    throw err;
  });
});
