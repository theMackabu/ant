const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const inner = [
  'const { spawnSync } = require("child_process");',
  'const result = spawnSync(process.execPath, ["-e", "console.log(\\"OUT\\"); console.error(\\"ERR\\");"], { stdio: "inherit" });',
  'if (result.status !== 0) process.exit(10);'
].join('\n');

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-spawnsync-inherit-'));
const innerPath = path.join(tmpDir, 'inner.cjs');
fs.writeFileSync(innerPath, inner);

const result = spawnSync(process.execPath, [innerPath], { encoding: 'utf8' });

assert(result.status === 0, `inner process exited ${result.status}`);
assert(result.stdout === 'OUT\n', `expected inherited stdout, got ${JSON.stringify(result.stdout)}`);
assert(result.stderr === 'ERR\n', `expected inherited stderr, got ${JSON.stringify(result.stderr)}`);

console.log('child_process.spawnSync stdio inherit ok');
