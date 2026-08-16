const { EventEmitter } = require('events');

const iterations = Number(process.argv[2] || 5_000_000);
const listenerCount = Number(process.argv[3] || 1);
const emitter = new EventEmitter();
let calls = 0;

emitter.setMaxListeners(0);
for (let i = 0; i < listenerCount; i++) {
  emitter.on('tick', value => {
    calls += value;
  });
}

for (let i = 0; i < 100_000; i++) emitter.emit('tick', 1);

calls = 0;
const start = performance.now();
for (let i = 0; i < iterations; i++) emitter.emit('tick', 1);
const elapsedMs = performance.now() - start;

console.log(JSON.stringify({
  iterations,
  listenerCount,
  calls,
  elapsedMs,
  dispatchesPerSecond: iterations / (elapsedMs / 1000),
  listenerCallsPerSecond: calls / (elapsedMs / 1000),
}));
