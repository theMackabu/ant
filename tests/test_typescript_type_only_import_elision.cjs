const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-ts-type-import-'));
const packageRoot = path.join(tmpRoot, 'node_modules', '@acme', 'ts-type-imports');
const scriptPath = path.join(tmpRoot, 'entry.ts');

fs.mkdirSync(path.join(packageRoot, 'util'), { recursive: true });

fs.writeFileSync(
  path.join(packageRoot, 'package.json'),
  JSON.stringify(
    {
      name: '@acme/ts-type-imports',
      type: 'module',
      exports: {
        import: './util/loader_mjs.mjs'
      }
    },
    null,
    2
  )
);

fs.writeFileSync(path.join(packageRoot, 'util', 'loader_mjs.mjs'), ['export const RuntimeThing = 42;', ''].join('\n'));

fs.writeFileSync(
  scriptPath,
  [
    "import { RuntimeThing, MissingType, MissingTypeTwo } from '@acme/ts-type-imports';",
    'const one = null as unknown as MissingType;',
    'const two = null as unknown as MissingTypeTwo;',
    'void one;',
    'void two;',
    'console.log(RuntimeThing);',
    ''
  ].join('\n')
);

const result = spawnSync(process.execPath, [scriptPath], {
  cwd: tmpRoot,
  encoding: 'utf8'
});

if (result.error) throw result.error;

assert(
  result.status === 0,
  `expected type-only imports in .ts entrypoint to be erased, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);
assert(result.stdout === '42\n', `expected stdout to be 42, got ${JSON.stringify(result.stdout)}`);
assert(
  !/does not provide an export named 'MissingType'/.test(result.stderr),
  `expected no missing named export error for type-only import, got stderr:\n${result.stderr}`
);

console.log('TypeScript type-only import elision works');
