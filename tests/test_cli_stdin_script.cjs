const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ant = path.resolve(process.execPath);
const source = `
"use strict";
globalThis.__stdinScriptProbe = 1;
console.log(this === globalThis && this.__stdinScriptProbe === 1);
`;

function run(args, input) {
  const result = spawnSync(ant, args, { input, encoding: 'utf8' });
  if (result.error) throw result.error;
  return result;
}

for (const args of [[], ['-']]) {
  const result = run(args, source);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout.trim(), 'true');
}

const commonjsResult = run(['--input-type=commonjs', '-'], source);
assert.equal(commonjsResult.status, 0, commonjsResult.stderr);
assert.equal(commonjsResult.stdout.trim(), 'false');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cli-stdin-script-'));
try {
  const file = path.join(tmp, 'entry.cjs');
  fs.writeFileSync(file, source);
  const result = run([file]);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout.trim(), 'false');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}

console.log('cli stdin uses script semantics');
