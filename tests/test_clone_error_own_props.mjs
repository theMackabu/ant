import assert from 'node:assert/strict';
import { MessageChannel, Worker } from 'node:worker_threads';

function makeError() {
  const error = new TypeError('bad', { cause: new RangeError('inner') });
  error.code = 'ERR_X';
  error.extra = { n: 1 };
  return error;
}

function checkMessageClone(error) {
  assert.ok(error instanceof TypeError);
  assert.equal(error.message, 'bad');
  assert.equal(error.code, undefined);
  assert.equal(error.extra, undefined);
  assert.deepEqual(Object.keys(error), []);
  assert.ok(error.cause instanceof RangeError);
  assert.equal(error.cause.message, 'inner');
  assert.equal(Object.getOwnPropertyDescriptor(error, 'cause').enumerable, false);
}

checkMessageClone(structuredClone(makeError()));

const { port1, port2 } = new MessageChannel();
checkMessageClone(await new Promise(resolve => {
  port2.once('message', resolve);
  port1.postMessage(makeError());
}));
port1.close();

const source = `
  const error = new TypeError('bad');
  error.code = 'ERR_X';
  error.extra = { n: 1 };
`;

const posted = await new Promise((resolve, reject) => {
  const worker = new Worker(`${source} require('node:worker_threads').parentPort.postMessage(error);`, { eval: true });
  worker.once('message', resolve);
  worker.once('error', reject);
});
assert.equal(posted.code, undefined);
assert.equal(posted.extra, undefined);

const thrown = await new Promise(resolve => {
  const worker = new Worker(`${source} throw error;`, { eval: true });
  worker.once('error', resolve);
});
assert.ok(thrown instanceof TypeError);
assert.equal(thrown.code, 'ERR_X');
assert.deepEqual(thrown.extra, { n: 1 });

console.log('clone error own props: ok');
