const processEvents = [];

function first() {
  processEvents.push('first');
}

function second() {
  processEvents.push('second');
}

process.removeAllListeners('codex-process-api');
process.on('codex-process-api', first);
process.prependListener('codex-process-api', second);

const listeners = process.listeners('codex-process-api');
if (!Array.isArray(listeners) || listeners.length !== 2) {
  console.log('FAIL: process.listeners should return the registered listeners');
  process.exit(1);
}

if (listeners[0] !== second || listeners[1] !== first) {
  console.log('FAIL: process.prependListener should place the listener first');
  process.exit(1);
}

const rawListeners = process.rawListeners('codex-process-api');
if (!Array.isArray(rawListeners) || rawListeners.length !== 2) {
  console.log('FAIL: process.rawListeners should mirror process.listeners for direct listeners');
  process.exit(1);
}

const eventNames = process.eventNames();
if (!Array.isArray(eventNames) || eventNames.indexOf('codex-process-api') === -1) {
  console.log('FAIL: process.eventNames should include active events');
  process.exit(1);
}

process.emit('codex-process-api');

if (processEvents.join(',') !== 'second,first') {
  console.log('FAIL: process.emit should honor prepended listener order');
  process.exit(1);
}

process.removeAllListeners('codex-process-api');
console.log('PASS');
