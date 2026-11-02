const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-debug-shell-'));
const scriptPath = path.join(tmpDir, 'shell.mjs');
fs.writeFileSync(
  scriptPath,
  'import {$} from "ant:shell";\n' +
  'const word = "shell-output";\n' +
  'console.log(await $`printf ${word}`.text());\n' +
  'console.log(await $`i=0; while [ $i -lt 2 ]; do printf $i; i=$((i + 1)); done`.text());\n'
);

const bin = path.resolve(__dirname, '..', 'build', 'ant');
const expected = 'shell-output\n01\n';

const plain = spawnSync(bin, [scriptPath], { env: { ...process.env, ANT_DEBUG: '' }, encoding: 'utf8' });
if (plain.error) throw plain.error;
assert(plain.status === 0, `plain shell process failed\n${plain.stderr}`);
assert(plain.stdout === expected, `unexpected stdout: ${plain.stdout}`);
assert(!plain.stderr.includes('[shell:compile]'), `shell source dump should be opt-in\n${plain.stderr}`);

const result = spawnSync(bin, [scriptPath], { env: { ...process.env, ANT_DEBUG: 'dump/compile:shell' }, encoding: 'utf8' });
if (result.error) throw result.error;
assert(result.status === 0, `shell process failed\n${result.stderr}`);
assert(result.stdout === expected, `unexpected stdout: ${result.stdout}`);

const dumps = result.stderr.split('[shell:compile] template JavaScript').slice(1);
assert(dumps.length === 2, `expected one dump per template\n${result.stderr}`);
const loop = dumps[1].split('\n')[1];
assert(/for\(;;\)/.test(loop) && loop.includes('break;'), `while loop should lower to a Silver loop\n${loop}`);
assert(!/R\.(while|loop|interpret)/.test(loop), `loop control flow must not be delegated to the runtime\n${loop}`);
assert(result.stderr.includes('[shell:compile] AST ('), `missing AST dump\n${result.stderr}`);

fs.rmSync(tmpDir, { recursive: true, force: true });
console.log('debug shell compile test passed');
