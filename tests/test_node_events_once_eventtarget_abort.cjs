const assert = require('node:assert');
const events = require('node:events');

class ObservedEventTarget extends EventTarget {
  constructor() {
    super();
    this.removedReady = false;
  }

  removeEventListener(type, listener, options) {
    if (type === 'ready' && typeof listener === 'function') {
      this.removedReady = true;
    }
    return super.removeEventListener(type, listener, options);
  }
}

(async () => {
  const target = new ObservedEventTarget();
  const controller = new AbortController();
  const promise = events.once(target, 'ready', { signal: controller.signal });

  controller.abort();

  await assert.rejects(promise);
  assert.strictEqual(target.removedReady, true);

  console.log('node-events-once-eventtarget-abort:ok');
})().catch((err) => {
  setTimeout(() => {
    throw err;
  });
});
