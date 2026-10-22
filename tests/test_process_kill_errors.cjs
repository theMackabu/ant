const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const child = spawnSync(process.execPath, ['-e', 'console.log(process.pid)'], { encoding: 'utf8' });
const dead = Number(child.stdout.trim());
assert.ok(Number.isInteger(dead) && dead > 0);

assert.throws(
  () => process.kill(dead, 0),
  err => err.code === 'ESRCH' && err.syscall === 'kill' && typeof err.errno === 'number' && err.message === 'kill ESRCH'
);

assert.strictEqual(process.kill(process.pid, 0), true);

console.log('ok');
