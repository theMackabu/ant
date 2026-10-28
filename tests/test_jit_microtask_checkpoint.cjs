'use strict';

const assert = require('node:assert');
const { EventEmitter } = require('node:events');

let violations = 0;
let calls = 0;

function handler(value) {
  let microtask = false;
  let tick = false;
  queueMicrotask(() => { microtask = true; });
  process.nextTick(() => { tick = true; });
  const parts = [value, value + 1].map(String);
  parts.push(Math.max(value, 3).toString());
  if (microtask || tick || parts.indexOf('x') !== -1) violations++;
  calls++;
}

const emitter = new EventEmitter();
emitter.on('event', handler);

let remaining = 3000;
function step() {
  emitter.emit('event', remaining);
  if (--remaining > 0) setImmediate(step);
  else finish();
}

function finish() {
  assert.strictEqual(calls, 3000);
  assert.strictEqual(violations, 0);
  console.log('jit microtask checkpoint ok');
}

setImmediate(step);
