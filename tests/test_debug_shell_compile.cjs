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
  'console.log(await $`true && printf conditional-output`.text());\n'
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
assert(
  result.stdout === 'shell-output\nconditional-output\n',
  `unexpected stdout: ${result.stdout}`
);
assert(
  result.stderr.includes('[shell:compile] JavaScript ('),
  `missing shell source header\n${result.stderr}`
);
assert(
  result.stderr.includes('return await __run(__ctx,__plan,0,__values);'),
  `missing specialized single-clause entry\n${result.stderr}`
);
assert(
  result.stderr.includes('if(__result.exitCode===0){') &&
    result.stderr.includes('__run(__ctx,__plan,1,__values)'),
  `missing specialized connector control flow\n${result.stderr}`
);
assert(
  result.stderr.includes('[shell:compile] __plan (') &&
    result.stderr.includes('"literal":"printf"') &&
    result.stderr.includes('"interpolation":0'),
  `missing compiled shell plan\n${result.stderr}`
);
assert(
  result.stderr.includes('[shell:invoke] bindings') &&
    result.stderr.includes('__run = [native sh_runtime_run]') &&
    result.stderr.includes('__finish = [native sh_runtime_finish]') &&
    result.stderr.includes('__ctx = { cwd: "') &&
    result.stderr.includes('__values = ["shell-output"]') &&
    result.stderr.includes('__plan = [compiled shell plan: 1 clause]'),
  `missing shell invocation bindings\n${result.stderr}`
);

const jsLines = result.stderr.split('\n').filter((line, index, lines) =>
  index > 0 && lines[index - 1].startsWith('[shell:compile] JavaScript')
);
assert(
  jsLines.every(line => !line.includes('"printf"') && !line.includes('[[[')),
  `static pipeline data should remain outside executable JavaScript\n${result.stderr}`
);

console.log('debug shell compile test passed');
