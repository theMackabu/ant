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

const mutationEvents = [];
function addedDuringEmit() {
  mutationEvents.push('added');
}
function addListenerDuringEmit() {
  mutationEvents.push('first');
  process.on('codex-process-mutation', addedDuringEmit);
}

process.on('codex-process-mutation', addListenerDuringEmit);
process.emit('codex-process-mutation');
if (mutationEvents.join(',') !== 'first') {
  console.log('FAIL: listeners added during process.emit should wait for the next emission');
  process.exit(1);
}

process.emit('codex-process-mutation');
if (mutationEvents.join(',') !== 'first,first,added') {
  console.log('FAIL: listeners added during process.emit should run on the next emission');
  process.exit(1);
}
process.removeAllListeners('codex-process-mutation');

const removalEvents = [];
function removedDuringEmit() {
  removalEvents.push('removed');
}
process.on('codex-process-removal', () => {
  removalEvents.push('first');
  process.off('codex-process-removal', removedDuringEmit);
});
process.on('codex-process-removal', removedDuringEmit);
process.emit('codex-process-removal');
if (removalEvents.join(',') !== 'first,removed') {
  console.log('FAIL: listener removal should not alter an active process.emit snapshot');
  process.exit(1);
}
process.removeAllListeners('codex-process-removal');

const nestedOnceEvents = [];
let nested = false;
process.on('codex-process-nested-once', () => {
  nestedOnceEvents.push('on');
  if (!nested) {
    nested = true;
    process.emit('codex-process-nested-once');
  }
});
process.once('codex-process-nested-once', () => {
  nestedOnceEvents.push('once');
});
process.emit('codex-process-nested-once');
if (nestedOnceEvents.join(',') !== 'on,on,once') {
  console.log('FAIL: nested process.emit should not invoke a once listener twice');
  process.exit(1);
}
process.removeAllListeners('codex-process-nested-once');

const symbolEvent = Symbol('codex-process-symbol');
let symbolCalls = 0;
function onSymbol(value) {
  symbolCalls += value;
}

process.on(symbolEvent, onSymbol);
if (!process.eventNames().includes(symbolEvent)) {
  console.log('FAIL: process.eventNames should preserve Symbol event keys');
  process.exit(1);
}
if (process.emit(symbolEvent, 2) !== true || symbolCalls !== 2) {
  console.log('FAIL: process.on/emit should support Symbol event keys');
  process.exit(1);
}
process.off(symbolEvent, onSymbol);
if (process.emit(symbolEvent, 2) !== false || symbolCalls !== 2) {
  console.log('FAIL: process.off should support Symbol event keys');
  process.exit(1);
}

process.once(symbolEvent, onSymbol);
process.removeAllListeners(symbolEvent);
if (process.emit(symbolEvent, 2) !== false || symbolCalls !== 2) {
  console.log('FAIL: process.removeAllListeners should support Symbol event keys');
  process.exit(1);
}

console.log('PASS');
