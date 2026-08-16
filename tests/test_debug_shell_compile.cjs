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
  'import {$} from "ant:shell";\nconsole.log(await $`printf shell-output`.text());\n'
);

const bin = path.resolve(__dirname, '..', 'build', 'ant');
const plain = spawnSync(bin, [scriptPath], {
  env: { ...process.env, ANT_DEBUG: '' },
  encoding: 'utf8',
});
if (plain.error) throw plain.error;
assert(plain.status === 0, `plain shell process failed\n${plain.stderr}`);
assert(
  !plain.stderr.includes('[shell:compile]'),
  `shell source dump should be opt-in\n${plain.stderr}`
);

const result = spawnSync(bin, [scriptPath], {
  env: { ...process.env, ANT_DEBUG: 'dump/compile:shell' },
  encoding: 'utf8',
});

if (result.error) throw result.error;
assert(result.status === 0, `shell process failed\n${result.stderr}`);
assert(result.stdout === 'shell-output\n', `unexpected stdout: ${result.stdout}`);
assert(
  result.stderr.includes('[shell:compile] JavaScript ('),
  `missing shell source header\n${result.stderr}`
);
assert(
  result.stderr.includes('__result=await __run(__ctx,'),
  `missing generated shell JavaScript\n${result.stderr}`
);
assert(
  result.stderr.includes('"printf"') && result.stderr.includes('"shell-output"'),
  `missing compiled command words\n${result.stderr}`
);

console.log('debug shell compile test passed');
