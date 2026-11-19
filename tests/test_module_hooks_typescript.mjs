import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { registerHooks } from 'node:module';
import { pathToFileURL } from 'node:url';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-hooks-ts-'));
try {
  const file = path.join(root, 'typed.ts');
  const dependency = path.join(root, 'dependency.ts');
  fs.writeFileSync(dependency, 'export const offset: number = 40;\n');
  fs.writeFileSync(file, "import { offset } from './dependency.ts';\nexport const value: number = offset + 2;\n");

  const seen = [];
  registerHooks({
    resolve(specifier, context, next) {
      return next(specifier, context);
    },
    load(url, context, next) {
      seen.push(path.basename(url));
      return next(url, context);
    }
  });

  const loaded = await import(pathToFileURL(file).href);
  assert.equal(loaded.value, 42);
  assert.deepEqual(seen, ['typed.ts', 'dependency.ts']);
  console.log('module-hooks-typescript-ok');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
