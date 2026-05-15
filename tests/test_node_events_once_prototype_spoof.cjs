const assert = require('node:assert');
const events = require('node:events');

const { EventEmitter } = events;

globalThis.onunhandledrejection = () => {};

(async () => {
  const timeout = setTimeout(() => {
    throw new Error('events.once prototype spoof rejection timed out');
  }, 50);

  try {
    let emitterRejected = false;
    try {
      await events.once(Object.create(EventEmitter.prototype), 'ready');
    } catch {
      emitterRejected = true;
    }
    assert.strictEqual(emitterRejected, true);

    let targetRejected = false;
    try {
      await events.once(Object.create(EventTarget.prototype), 'ready');
    } catch {
      targetRejected = true;
    }
    assert.strictEqual(targetRejected, true);
  } finally {
    clearTimeout(timeout);
  }

  console.log('node-events-once-prototype-spoof:ok');
})().catch((err) => {
  setTimeout(() => {
    throw err;
  });
});
