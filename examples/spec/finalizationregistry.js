import { test, summary } from './helpers.js';

console.log('FinalizationRegistry Tests\n');

let callbackCalled = false;
const fr = new FinalizationRegistry(function(heldValue) {
  callbackCalled = true;
});

test('finalizationregistry instanceof', fr instanceof FinalizationRegistry, true);
test('finalizationregistry prototype', Object.getPrototypeOf(fr) === FinalizationRegistry.prototype, true);
test('finalizationregistry has register', typeof fr.register, 'function');
test('finalizationregistry has unregister', typeof fr.unregister, 'function');

const target = {};
const token = {};
fr.register(target, 'held value', token);

test('finalizationregistry register returns undefined', fr.register({}, 'test'), undefined);

const unregisterResult = fr.unregister(token);
test('finalizationregistry unregister returns boolean', typeof unregisterResult, 'boolean');
test('finalizationregistry unregister found token', unregisterResult, true);

const notFoundResult = fr.unregister({});
test('finalizationregistry unregister not found', notFoundResult, false);

summary();
