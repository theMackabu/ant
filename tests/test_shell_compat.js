import assert from 'node:assert';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { $ } from 'ant:shell';

const here = path.dirname(fileURLToPath(import.meta.url));

if (!process.env.ANT_SHELL_COMPAT_CHILD) {
  const env = { PATH: process.env.PATH, LC_ALL: 'C', HOME: '/nonexistent-home', _: '/fixture-shell', ANT_SHELL_COMPAT_CHILD: '1' };
  const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url)], { env, cwd: '/tmp', encoding: 'utf8' });
  process.stdout.write(child.stdout);
  process.stderr.write(child.stderr);
  assert.strictEqual(child.status, 0, 'bash compatibility cases failed');
} else {
  const cases = JSON.parse(fs.readFileSync(path.join(here, 'fixtures', 'shell_bash_compat.json'), 'utf8'));
  const failures = [];
  const check = (mode, src, expected, result) => {
    const actual = { out: result.text(), code: result.exitCode, err: result.stderr.length > 0 };
    if (actual.out !== expected.out || actual.code !== expected.code || actual.err !== expected.err) {
      failures.push(`[${mode}] ${src}\n  expected ${JSON.stringify({ out: expected.out, code: expected.code, err: expected.err })}\n  actual   ${JSON.stringify(actual)}`);
    }
  };
  for (const item of cases) {
    check('default', item.src, item, await $`eval ${item.src}`.nothrow());
    check('posix', item.src, item.posix || item, await $`eval ${'set -o posix\n' + item.src}`.nothrow());
  }
  if (failures.length) {
    console.error(failures.join('\n'));
    console.error(`${failures.length} of ${cases.length * 2} bash compatibility checks failed`);
    process.exit(1);
  }
  console.log(`ant:shell bash compatibility: ${cases.length} cases passed in default and POSIX mode`);
}
