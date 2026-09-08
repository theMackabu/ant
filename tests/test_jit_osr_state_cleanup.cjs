const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function captured(value) { value = value; return () => value; }
function finiteOsr() {
  let sum = 0;
  for (let i = 0; i < 1000; i++) sum += i;
  return sum;
}
for (let i = 0; i < 400; i++) assert.strictEqual(captured(42)(), 42);
assert.strictEqual(finiteOsr(), 499500);
assert.strictEqual(captured()(), undefined, 'OSR must not invent omitted arguments');

const source = `
function captured(value) { value = value; return () => value; }
for(let i=0;i<110;i++) captured(42);
let entries=0;
function selfLoop() {
  if(++entries>1) return;
  for(;;) {}
}
console.log('ready');
selfLoop();
console.log('unexpected return', entries, captured()());
`;
const child = spawnSync(process.execPath, ['-e', source], {
  encoding: 'utf8',
  timeout: 1000,
  killSignal: 'SIGKILL',
});
assert.match(child.stdout, /ready/);
assert.doesNotMatch(child.stdout, /unexpected return/, 'OSR must not restart the prologue');
assert.strictEqual(child.error && child.error.code, 'ETIMEDOUT', 'self-loop must remain in the loop');
console.log('jit osr state cleanup: ok');
