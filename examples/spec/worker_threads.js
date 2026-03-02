import { test, testDeep, summary } from './helpers.js';
import {
  Worker,
  MessageChannel,
  isMainThread,
  parentPort,
  workerData,
  receiveMessageOnPort,
  setEnvironmentData,
  getEnvironmentData,
  moveMessagePortToContext
} from 'node:worker_threads';

console.log('Worker Threads Tests\n');

async function run() {
  test('isMainThread in main context', isMainThread, true);
  test('parentPort is null in main context', parentPort, null);
  test('workerData is undefined in main context', workerData, undefined);

  const ch1 = new MessageChannel();
  test('MessageChannel has port1', typeof ch1.port1, 'object');
  test('MessageChannel has port2', typeof ch1.port2, 'object');

  ch1.port1.postMessage({ hello: 'queue' });
  const queued = receiveMessageOnPort(ch1.port2);
  testDeep('receiveMessageOnPort returns message envelope', queued, { message: { hello: 'queue' } });
  test('receiveMessageOnPort returns undefined when empty', receiveMessageOnPort(ch1.port2), undefined);

  const ch2 = new MessageChannel();
  const seen = [];
  ch2.port1.postMessage('queued-before-listener');
  ch2.port2.on('message', msg => {
    seen.push(`on:${msg}`);
  });
  ch2.port2.once('message', msg => {
    seen.push(`once:${msg}`);
  });
  ch2.port1.postMessage('next');
  ch2.port1.postMessage('final');
  testDeep('MessagePort queue drains in FIFO order', seen, ['on:queued-before-listener', 'on:next', 'once:next', 'on:final']);

  let onmessageData = undefined;
  ch2.port2.onmessage = event => {
    onmessageData = event.data;
  };
  ch2.port1.postMessage('event-data');
  test('MessagePort onmessage receives event.data', onmessageData, 'event-data');

  const moved = moveMessagePortToContext(ch2.port1, globalThis);
  test('moveMessagePortToContext returns same port', moved, ch2.port1);

  const shared = { answer: 42, ok: true };
  setEnvironmentData('shared-key', shared);
  testDeep('getEnvironmentData in main context', getEnvironmentData('shared-key'), shared);

  const workerUrl = new URL('./worker_threads.worker.mjs', import.meta.url);
  const payload = { hello: 'world', n: 42, arr: [1, 2, 3] };

  const worker = new Worker(workerUrl, { workerData: payload });
  test('Worker constructor returns object', typeof worker, 'object');
  test('Worker has once method', typeof worker.once, 'function');
  test('Worker has terminate method', typeof worker.terminate, 'function');

  worker.unref();
  worker.ref();

  const message = await new Promise((resolve, reject) => {
    worker.once('message', resolve);
    worker.once('exit', code => {
      if (code !== 0) reject(new Error(`worker exited early with code ${code}`));
    });
  });

  test('worker replied success flag', message.ok, true);
  test('worker isMainThread false in worker context', message.isMainThread, false);
  test('worker has parentPort in worker context', message.hasParentPort, true);
  testDeep('workerData round-trip', message.workerData, payload);
  testDeep('worker getEnvironmentData snapshot', message.environmentData, shared);

  const exitCode = await worker.terminate();
  test('terminate resolves to numeric exit code', typeof exitCode, 'number');

  summary();
}

void run();
