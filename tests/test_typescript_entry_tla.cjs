const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-ts-entry-tla-'));
const scriptPath = path.join(tmpRoot, 'entry.ts');

fs.writeFileSync(
  scriptPath,
  [
    'const message: string = await Promise.resolve("ts tla ok");',
    'console.log(message);',
    '',
  ].join('\n')
);

const result = spawnSync(process.execPath, [scriptPath], {
  encoding: 'utf8',
});

if (result.error) throw result.error;

assert(
  result.status === 0,
  `expected direct .ts entry with top-level await to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);
assert(
  result.stdout === 'ts tla ok\n',
  `expected stdout to be ts tla ok, got ${JSON.stringify(result.stdout)}`
);
assert(
  !/strip failed|await is only allowed within async functions/i.test(result.stderr),
  `expected no TypeScript strip/TLA parse error, got stderr:\n${result.stderr}`
);

console.log('direct TypeScript entrypoint top-level await works');
