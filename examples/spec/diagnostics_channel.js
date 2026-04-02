import { test, testThrows, testDeep, summary } from './helpers.js';
import diagnosticsChannel from 'node:diagnostics_channel';

console.log('diagnostics_channel\n');

function collectEvents(tracing) {
  const events = [];

  for (const name of ['start', 'end', 'asyncStart', 'asyncEnd', 'error']) {
    tracing[name].subscribe(message => {
      events.push({ name, message: { ...message } });
    });
  }

  return events;
}

test('exports channel', typeof diagnosticsChannel.channel, 'function');
test('exports tracingChannel', typeof diagnosticsChannel.tracingChannel, 'function');

const symbolName = Symbol('diagnostics');
test(
  'channel caches string names',
  diagnosticsChannel.channel('spec:diagnostics:string') === diagnosticsChannel.channel('spec:diagnostics:string'),
  true
);
test('channel accepts symbol names', diagnosticsChannel.channel(symbolName).name, symbolName);

testThrows('channel rejects invalid names', () => diagnosticsChannel.channel(123));
testThrows('subscribe rejects non-functions', () => diagnosticsChannel.channel('spec:diagnostics:invalid-subscription').subscribe(123));

const storeChannel = diagnosticsChannel.channel('spec:diagnostics:store');
const store = {
  run(_value, fn) {
    return fn();
  }
};
test('hasSubscribers starts false', storeChannel.hasSubscribers, false);
storeChannel.bindStore(store);
test('bindStore contributes to hasSubscribers', storeChannel.hasSubscribers, true);
storeChannel.unbindStore(store);
test('unbindStore clears hasSubscribers', storeChannel.hasSubscribers, false);

const snapshotChannel = diagnosticsChannel.channel('spec:diagnostics:snapshot');
const snapshotCalls = [];
const lateSubscriber = () => snapshotCalls.push('late');
snapshotChannel.subscribe(() => {
  snapshotCalls.push('first');
  snapshotChannel.subscribe(lateSubscriber);
});
snapshotChannel.subscribe(() => snapshotCalls.push('second'));
snapshotChannel.publish({});
snapshotChannel.publish({});
testDeep('publish snapshots subscribers', snapshotCalls, ['first', 'second', 'first', 'second', 'late']);

const syncTracing = diagnosticsChannel.tracingChannel('spec:diagnostics:sync');
const syncEvents = collectEvents(syncTracing);
test(
  'traceSync returns result',
  syncTracing.traceSync(() => 42, { phase: 'sync' }),
  42
);
testDeep('traceSync publishes start/end', syncEvents, [
  { name: 'start', message: { phase: 'sync' } },
  { name: 'end', message: { phase: 'sync', result: 42 } }
]);

const promiseTracing = diagnosticsChannel.tracingChannel('spec:diagnostics:promise');
const promiseEvents = collectEvents(promiseTracing);
test('tracePromise resolves result', await promiseTracing.tracePromise(() => Promise.resolve(7), { phase: 'promise' }), 7);
testDeep('tracePromise publishes end before async events', promiseEvents, [
  { name: 'start', message: { phase: 'promise' } },
  { name: 'end', message: { phase: 'promise' } },
  { name: 'asyncStart', message: { phase: 'promise', result: 7 } },
  { name: 'asyncEnd', message: { phase: 'promise', result: 7 } }
]);

const callbackTracing = diagnosticsChannel.tracingChannel('spec:diagnostics:callback');
const callbackEvents = collectEvents(callbackTracing);
let callbackError = '';
const callbackReturn = callbackTracing.traceCallback(
  callback => {
    try {
      callback(null, 9);
    } catch (err) {
      callbackError = err.message;
    }
    return 'outer-result';
  },
  -1,
  { phase: 'callback' },
  undefined,
  () => {
    throw new Error('callback boom');
  }
);
test('traceCallback returns outer result', callbackReturn, 'outer-result');
test('traceCallback uses the last argument by default', callbackError, 'callback boom');
testDeep('traceCallback still publishes asyncEnd when callback throws', callbackEvents, [
  { name: 'start', message: { phase: 'callback' } },
  { name: 'asyncStart', message: { phase: 'callback', result: 9 } },
  { name: 'asyncEnd', message: { phase: 'callback', result: 9 } },
  { name: 'end', message: { phase: 'callback', result: 9 } }
]);

summary();
