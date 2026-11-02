import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { $ } from 'ant:shell';

const here = path.dirname(fileURLToPath(import.meta.url));
const bash = process.env.BASH_BIN || 'bash';
const posix = process.argv.includes('--posix');
process.env.LC_ALL = 'C';
const env = { ...process.env };
let pass = 0;
let fail = 0;

process.chdir('/tmp');
for (const file of fs.readdirSync(path.join(here, 'cases')).sort()) {
  const cases = fs.readFileSync(path.join(here, 'cases', file), 'utf8').split('\n---\n').map(s => s.replace(/\n$/, ''));
  for (const raw of cases) {
    const src = posix ? 'set -o posix\n' + raw : raw;
    const r = spawnSync(bash, ['-c', src], { encoding: 'utf8', env });
    const res = await $`eval ${src}`.nothrow();
    const actual = { out: res.text(), code: res.exitCode, err: res.stderr.length > 0 };
    if (actual.out === r.stdout && actual.code === r.status && actual.err === (r.stderr !== '')) {
      pass++;
      continue;
    }
    fail++;
    console.log(`--- ${file}: ${raw.split('\n')[0].slice(0, 100)}`);
    console.log(`  bash: ${JSON.stringify({ out: r.stdout, code: r.status, err: r.stderr })}`);
    console.log(`  ant:  ${JSON.stringify({ out: actual.out, code: actual.code, err: res.stderr.toString() })}`);
  }
}
console.log(`${posix ? 'posix' : 'default'} mode: ${pass} match, ${fail} differ`);
