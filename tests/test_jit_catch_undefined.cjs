const assert = require('node:assert');

function capture(action) {
  try {
    action();
    return { caught: false };
  } catch (value) {
    return { caught: true, value };
  }
}

const warmup = new Error('warmup');
for (let i = 0; i < 150; i++) {
  const result = capture(() => { throw warmup; });
  assert.strictEqual(result.value, warmup);
}

for (const reason of [undefined, null, false, 0, 'reason', Symbol('reason'), new Error('reason')]) {
  const result = capture(() => { throw reason; });
  assert.strictEqual(result.caught, true);
  assert.strictEqual(result.value, reason);
}
queueMicrotask(() => console.log('JIT catch values ok'));
