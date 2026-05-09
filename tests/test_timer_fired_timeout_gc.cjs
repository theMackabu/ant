const assert = require('node:assert');

let ref;

{
  let callback = () => {};
  ref = new WeakRef(callback);
  setTimeout(callback, 0);
  callback = null;
}

function forceAllocations() {
  for (let i = 0; i < 200000; i++) {
    ({ i, value: `timer-gc-${i}` });
  }
}

setTimeout(() => {
  forceAllocations();
  assert.strictEqual(ref.deref(), undefined);
  console.log('timer:fired-timeout-gc:ok');
}, 10);
