const assert = require('node:assert');
const { execFile, spawn, spawnSync } = require('node:child_process');

for (const invoke of [
  () => spawn('printf\0ignored', []),
  () => spawn('printf', ['before\0after']),
  () => spawnSync('printf\0ignored', []),
  () => spawnSync('printf', ['before\0after']),
  () => execFile('printf\0ignored', []),
  () => execFile('printf', ['before\0after']),
]) {
  assert.throws(invoke, /NUL/, TypeError);
}

console.log('child_process rejects embedded NUL bytes');
