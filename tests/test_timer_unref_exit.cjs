const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function run(source) {
  return spawnSync(process.execPath, ['-e', source], {
    encoding: 'utf8',
    timeout: 1000,
  });
}

const unrefInterval = run([
  'const interval = setInterval(() => {',
  '  console.log("unexpected interval");',
  '}, 100);',
  'assertHasRef(interval, true);',
  'interval.unref();',
  'assertHasRef(interval, false);',
  'console.log("end");',
  '',
  'function assertHasRef(timer, expected) {',
  '  if (typeof timer.hasRef !== "function") throw new Error("missing hasRef");',
  '  if (timer.hasRef() !== expected) throw new Error("hasRef mismatch");',
  '}',
].join('\n'));

if (unrefInterval.error) throw unrefInterval.error;
assert.strictEqual(
  unrefInterval.status,
  0,
  `unref interval should not keep the event loop alive\nstdout:\n${unrefInterval.stdout}\nstderr:\n${unrefInterval.stderr}`
);
assert.strictEqual(unrefInterval.stdout, 'end\n');

const refAfterUnref = run([
  'const timeout = setTimeout(() => {',
  '  console.log("fired");',
  '}, 10);',
  'timeout.unref();',
  'if (timeout.hasRef()) throw new Error("expected unref timer");',
  'timeout.ref();',
  'if (!timeout.hasRef()) throw new Error("expected ref timer");',
].join('\n'));

if (refAfterUnref.error) throw refAfterUnref.error;
assert.strictEqual(
  refAfterUnref.status,
  0,
  `ref() should restore timer event-loop liveness\nstdout:\n${refAfterUnref.stdout}\nstderr:\n${refAfterUnref.stderr}`
);
assert.strictEqual(refAfterUnref.stdout, 'fired\n');

console.log('timer unref exit ok');
