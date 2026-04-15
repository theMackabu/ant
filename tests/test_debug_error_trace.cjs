const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function runCase(bin, dir, file, source) {
  const scriptPath = path.join(dir, file);
  fs.writeFileSync(scriptPath, source);

  const result = spawnSync(bin, [scriptPath], {
    env: { ...process.env, ANT_DEBUG: 'dump/errors:trace' },
    encoding: 'utf8',
  });

  if (result.error) throw result.error;
  return { scriptPath, ...result };
}

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-debug-errors-'));
const bin = path.resolve(__dirname, '..', 'build', 'ant');

const thrown = runCase(
  bin,
  tmpDir,
  'throw-project-path.cjs',
  "throw new Error('Project path is required');\n"
);
assert(thrown.status !== 0, 'throw case should fail');
assert(
  thrown.stderr.includes('[ant-debug:error] throw Error: Project path is required'),
  `expected throw trace in stderr\n${thrown.stderr}`
);
assert(
  thrown.stderr.includes(`site: ${thrown.scriptPath}:1:`),
  `expected throw site in stderr\n${thrown.stderr}`
);

const missing = runCase(
  bin,
  tmpDir,
  'missing-bailing.cjs',
  "console.log(bailing);\n"
);
assert(missing.status !== 0, 'missing identifier case should fail');
assert(
  missing.stderr.includes('[ant-debug:error] create ReferenceError'),
  `expected ReferenceError trace in stderr\n${missing.stderr}`
);
assert(
  missing.stderr.includes('bailing'),
  `expected missing identifier name in stderr\n${missing.stderr}`
);

const constAssign = runCase(
  bin,
  tmpDir,
  'const-assign.cjs',
  "const value = 1;\nvalue = 2;\n"
);
assert(constAssign.status !== 0, 'const assignment case should fail');
assert(
  constAssign.stderr.includes('[ant-debug:error] create TypeError: Assignment to constant variable'),
  `expected const assignment trace in stderr\n${constAssign.stderr}`
);
assert(
  constAssign.stderr.includes(`site: ${constAssign.scriptPath}:2:`),
  `expected const assignment site in stderr\n${constAssign.stderr}`
);

console.log('debug error trace test passed');
