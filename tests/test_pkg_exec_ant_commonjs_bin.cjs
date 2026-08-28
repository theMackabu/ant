const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform === 'win32') {
  console.log('skipping antx CommonJS bin test on win32');
  process.exit(0);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'antx-commonjs-bin-'));
const ant = path.resolve(process.execPath);
const packageDir = path.join(tmp, 'node_modules', 'commonjs-bin');
const packageBinDir = path.join(packageDir, 'bin');
const packageBin = path.join(packageBinDir, 'commonjs-bin');
const modulePackageDir = path.join(tmp, 'node_modules', 'module-bin');
const modulePackageBinDir = path.join(modulePackageDir, 'bin');
const modulePackageBin = path.join(modulePackageBinDir, 'module-bin');
const binDir = path.join(tmp, 'node_modules', '.bin');
const antx = path.join(tmp, 'antx');

try {
  fs.mkdirSync(packageBinDir, { recursive: true });
  fs.mkdirSync(binDir, { recursive: true });
  fs.writeFileSync(
    path.join(packageDir, 'package.json'),
    JSON.stringify({ name: 'commonjs-bin', bin: 'bin/commonjs-bin' })
  );
  fs.writeFileSync(
    packageBin,
    [
      '#!/usr/bin/env node',
      "const path = require('node:path');",
      "console.log(`commonjs bin ${path.basename(__filename)} ${process.argv[2]}`);",
      '',
    ].join('\n')
  );
  fs.chmodSync(packageBin, 0o755);
  fs.symlinkSync(path.relative(binDir, packageBin), path.join(binDir, 'commonjs-bin'));

  fs.mkdirSync(modulePackageBinDir, { recursive: true });
  fs.writeFileSync(
    path.join(modulePackageDir, 'package.json'),
    JSON.stringify({ name: 'module-bin', type: 'module', bin: 'bin/module-bin' })
  );
  fs.writeFileSync(
    modulePackageBin,
    [
      '#!/usr/bin/env node',
      "import { basename } from 'node:path';",
      "console.log(`module bin ${basename(import.meta.filename)} ${process.argv[2]}`);",
      '',
    ].join('\n')
  );
  fs.chmodSync(modulePackageBin, 0o755);
  fs.symlinkSync(path.relative(binDir, modulePackageBin), path.join(binDir, 'module-bin'));
  fs.symlinkSync(ant, antx);

  const options = {
    cwd: tmp,
    encoding: 'utf8',
    env: {
      ...process.env,
      PATH: `${path.dirname(ant)}${path.delimiter}${process.env.PATH || ''}`,
    },
  };

  const commonJsResult = spawnSync(antx, ['--ant', 'commonjs-bin', 'ok'], options);
  if (commonJsResult.error) throw commonJsResult.error;

  assert.strictEqual(
    commonJsResult.status,
    0,
    `antx --ant CommonJS bin failed\nstdout:\n${commonJsResult.stdout}\nstderr:\n${commonJsResult.stderr}`
  );
  assert.strictEqual(commonJsResult.stdout, 'commonjs bin commonjs-bin ok\n');

  const moduleResult = spawnSync(antx, ['--ant', 'module-bin', 'ok'], options);
  if (moduleResult.error) throw moduleResult.error;

  assert.strictEqual(
    moduleResult.status,
    0,
    `antx --ant module bin failed\nstdout:\n${moduleResult.stdout}\nstderr:\n${moduleResult.stderr}`
  );
  assert.strictEqual(moduleResult.stdout, 'module bin module-bin ok\n');

  console.log('antx --ant detects extensionless package bin formats');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
