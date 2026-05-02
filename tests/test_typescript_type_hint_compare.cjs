const { spawnSync } = require('child_process');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function runFixture(file) {
  const result = spawnSync(process.execPath, [file], {
    encoding: 'utf8',
  });

  if (result.error) throw result.error;

  const stdout = result.stdout.replace(/\x1b\[[0-9;]*m/g, '');
  assert(
    result.status === 0,
    `expected ${path.basename(file)} to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );

  return stdout;
}

const fixtureDir = path.join(__dirname, 'fixtures');
const tsPath = path.join(fixtureDir, 'type_hints_compare.ts');
const jsPath = path.join(fixtureDir, 'type_hints_compare.js');

const tsOut = runFixture(tsPath);
const jsOut = runFixture(jsPath);

assert(
  tsOut === jsOut,
  `expected TypeScript and JavaScript comparison fixtures to match\n.ts:\n${tsOut}\n.js:\n${jsOut}`
);
assert(
  tsOut === 'rounds=10000\nchecksum=262549.128\nfallback=typescript\n',
  `unexpected comparison output: ${JSON.stringify(tsOut)}`
);

console.log('TypeScript type-hint fixture matches JavaScript fixture');
