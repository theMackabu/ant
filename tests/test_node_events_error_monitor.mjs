import assert from 'node:assert';
import eventsDefault, { EventEmitter, errorMonitor } from 'node:events';

assert.strictEqual(typeof errorMonitor, 'symbol');
assert.strictEqual(errorMonitor.description, 'events.errorMonitor');
assert.strictEqual(eventsDefault.errorMonitor, errorMonitor);
assert.strictEqual(EventEmitter.errorMonitor, errorMonitor);

console.log('node-events-error-monitor:ok');
