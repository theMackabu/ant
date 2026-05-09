const assert = require('node:assert');
const events = require('node:events');

const { EventEmitter } = events;

assert.strictEqual(typeof events.getEventListeners, 'function');
assert.strictEqual(EventEmitter.getEventListeners, events.getEventListeners);

{
  const emitter = new EventEmitter();
  function onceListener() {}
  function onListener() {}

  emitter.once('ready', onceListener);
  emitter.on('ready', onListener);

  const listeners = events.getEventListeners(emitter, 'ready');
  assert.deepStrictEqual(listeners, [onceListener, onListener]);

  listeners.length = 0;
  assert.deepStrictEqual(events.getEventListeners(emitter, 'ready'), [onceListener, onListener]);
}

{
  const target = new EventTarget();
  function first() {}
  function second() {}

  target.addEventListener('open', first);
  target.addEventListener('open', second, { once: true });

  assert.deepStrictEqual(events.getEventListeners(target, 'open'), [first, second]);

  target.removeEventListener('open', first);
  assert.deepStrictEqual(events.getEventListeners(target, 'open'), [second]);
}

{
  const fake = {
    listeners(name) {
      return [name, 'from-fake'];
    },
  };

  assert.deepStrictEqual(events.getEventListeners(fake, 'custom'), ['custom', 'from-fake']);
}

assert.deepStrictEqual(events.getEventListeners(new EventEmitter(), 1), []);

let invalidTargetThrew = false;
try {
  events.getEventListeners({});
} catch (error) {
  invalidTargetThrew = error instanceof TypeError || error.name === 'TypeError';
}
assert.strictEqual(invalidTargetThrew, true);

console.log('node-events-get-event-listeners:ok');
