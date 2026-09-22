// A fired timeout must drop its callback: the timer clears its own reference,
// so anything still holding the closure afterwards is a leak.
//
// The check retries because the collector scans the C stack conservatively. A
// dead copy of the callback pointer can still sit in the native frames that
// dispatched the timer, and that counts as a root until those frames are
// reused, which the next event-loop turn does. Whether the copy survives the
// first collection depends on how the compiler laid out those frames, so a
// single check passes or fails by build flavour (it passes under PGO and LTO,
// and failed at -O3 without them). Retrying tests the property we actually
// care about: a real leak is still reachable after every turn below.
const assert = require('node:assert');

const TURNS = 5;

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

let turn = 0;

function check() {
  forceAllocations();
  if (ref.deref() === undefined) {
    console.log(`timer:fired-timeout-gc:ok (turn ${turn})`);
    return;
  }

  assert.ok(turn < TURNS, `fired timeout callback still reachable after ${TURNS+1} turns`);
  turn++;
  setTimeout(check, 0);
}

setTimeout(check, 10);
