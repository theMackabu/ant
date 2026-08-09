// Generational closures: dead-young closures must be reclaimed by minor GC
// (bounded RSS under pure churn), while closures that stay reachable — via
// arrays, emitter listener sidecars, or abort-signal sidecars — must survive
// the minors that run between creation and use.
let failed = 0;
const check = (n, ok, detail) => {
  if (!ok) failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'} ${n}${ok ? '' : ` (${detail})`}`);
};

function make(i) {
  let x = i;
  return () => x;
}

// pure churn: every closure + upvalue dies immediately; the harness mem
// wrapper asserts the RSS bound (unbounded arena growth here is ~700MB)
let sink = 0;
for (let i = 0; i < 5_000_000; i++) sink += make(i)();
check('churn sum', sink === 12_499_997_500_000, `got ${sink}`);

// Bound closures own a malloc'd argv in the closure union. Churn both the
// bound payload and ordinary closures so GC stress can repeatedly sweep slots
// that have already been threaded through the arena free list.
function add(a, b) {
  return a + b;
}
let boundSink = 0;
for (let i = 0; i < 100_000; i++) boundSink += add.bind(null, i)(1);
check('bound argv churn', boundSink === 5_000_050_000, `got ${boundSink}`);

// retained closures interleaved with churn that drives many minors
const kept = [];
for (let i = 0; i < 100_000; i++) {
  kept.push(make(i));
  for (let j = 0; j < 10; j++) make(j)();
}
let sum = 0;
for (let i = 0; i < kept.length; i++) sum += kept[i]();
check('retained closures', sum === (99_999 * 100_000) / 2, `got ${sum}`);
kept.length = 0;

// young once-listener stored in an old emitter's C-side sidecar, invoked
// after heavy churn (regression: examples/demo/event_loop.js beforeExit
// SIGSEGV — sidecar stores bypassed the old->young write barrier)
const { EventEmitter } = require('node:events');
const em = new EventEmitter();
let fired = 0;
for (let r = 0; r < 25; r++) {
  em.once('tick', (v) => { fired += v; });
  for (let j = 0; j < 100_000; j++) make(j)();
  em.emit('tick', 1);
}
check('emitter once after churn', fired === 25, `fired ${fired}`);

// same shape for the abort-signal sidecars: direct listener and an
// AbortSignal.any composite held only by a source signal's follower list
const ac = new AbortController();
let direct = false;
ac.signal.addEventListener('abort', () => { direct = true; });
for (let j = 0; j < 1_000_000; j++) make(j)();
const dep = AbortSignal.any([ac.signal]);
let composite = false;
dep.addEventListener('abort', () => { composite = true; });
for (let j = 0; j < 1_000_000; j++) make(j)();
ac.abort();
check('abort listeners after churn', direct && composite, `direct=${direct} any=${composite}`);

if (failed) { console.log(`${failed} FAILED`); process.exit(1); }
console.log('OK');
