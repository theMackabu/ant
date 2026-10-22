import assert from 'node:assert/strict';
import { MessageChannel } from 'node:worker_threads';

function makeViews() {
  const buffer = new ArrayBuffer(8, { maxByteLength: 64 });
  return {
    tracking: new Uint8Array(buffer),
    trackingOffset: new Uint16Array(buffer, 2),
    fixed: new Uint8Array(buffer, 0, 4),
    view: new DataView(buffer),
    viewOffset: new DataView(buffer, 4),
    fixedView: new DataView(buffer, 0, 2)
  };
}

function lengthsAfterGrow(views) {
  views.tracking.buffer.resize(16);
  const out = Object.fromEntries(Object.entries(views).map(([key, view]) => [key, view.byteLength]));
  views.tracking.buffer.resize(8);
  return out;
}

const expected = { tracking: 16, trackingOffset: 14, fixed: 4, view: 16, viewOffset: 12, fixedView: 2 };

assert.deepEqual(lengthsAfterGrow(makeViews()), expected);

const cloned = structuredClone(makeViews());
assert.equal(cloned.tracking.buffer, cloned.view.buffer);
assert.deepEqual(lengthsAfterGrow(cloned), expected);

const { port1, port2 } = new MessageChannel();
const received = await new Promise(resolve => {
  port2.once('message', resolve);
  port1.postMessage(makeViews());
});
port1.close();

assert.equal(received.tracking.buffer, received.view.buffer);
assert.deepEqual(lengthsAfterGrow(received), expected);

console.log('clone length-tracking views: ok');
