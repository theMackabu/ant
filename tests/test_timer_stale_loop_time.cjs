const assert = require('node:assert');

function busy(ms) {
  const end = Date.now() + ms;
  while (Date.now() < end);
}

busy(120);
const timeoutStart = performance.now();
setTimeout(() => {
  const elapsed = performance.now() - timeoutStart;
  assert.ok(elapsed >= 45, `setTimeout(50) fired after ${elapsed.toFixed(1)}ms`);

  busy(120);
  const intervalStart = performance.now();
  const id = setInterval(() => {
    clearInterval(id);
    const waited = performance.now() - intervalStart;
    assert.ok(waited >= 45, `setInterval(50) fired after ${waited.toFixed(1)}ms`);

    const timer = setTimeout(() => {
      const refreshed = performance.now() - refreshStart;
      assert.ok(refreshed >= 45, `refresh(50) fired after ${refreshed.toFixed(1)}ms`);
      console.log('timer stale loop time OK');
    }, 50);
    busy(120);
    const refreshStart = performance.now();
    timer.refresh();
  }, 50);
}, 50);
