const assert = require("assert");

(async () => {
  const sab = new SharedArrayBuffer(4);
  const view = new Int32Array(sab);

  Atomics.store(view, 0, 7);

  const timedOut = Atomics.waitAsync(view, 0, 7, 1);
  assert.equal(timedOut.async, true);
  assert.equal(await timedOut.value, "timed-out");

  const pending = Atomics.waitAsync(view, 0, 7, 1000);
  assert.equal(pending.async, true);
  assert.equal(Atomics.notify(view, 0, 1), 1);
  assert.equal(await pending.value, "ok");

  console.log("ok");
})().catch((err) => {
  console.error(err && err.stack ? err.stack : err);
  process.exit(1);
});
