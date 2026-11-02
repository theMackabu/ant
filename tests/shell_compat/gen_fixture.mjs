import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const bash = process.env.BASH_BIN || 'bash';
const env = { PATH: process.env.PATH, LC_ALL: 'C', HOME: '/nonexistent-home', _: '/fixture-shell' };
const out = [];

for (const file of fs.readdirSync(path.join(here, 'cases')).sort()) {
  const cases = fs.readFileSync(path.join(here, 'cases', file), 'utf8').split('\n---\n').map(s => s.replace(/\n$/, ''));
  for (const src of cases) {
    const r = spawnSync(bash, ['-c', src], { encoding: 'utf8', env, cwd: '/tmp' });
    const p = spawnSync(bash, ['-c', 'set -o posix\n' + src], { encoding: 'utf8', env, cwd: '/tmp' });
    const item = { src, out: r.stdout, code: r.status, err: r.stderr !== '' };
    if (p.stdout !== r.stdout || p.status !== r.status || (p.stderr !== '') !== item.err) item.posix = { out: p.stdout, code: p.status, err: p.stderr !== '' };
    out.push(item);
  }
}

const target = path.join(here, '..', 'fixtures', 'shell_bash_compat.json');
fs.writeFileSync(target, JSON.stringify(out, null, 1));
console.log(`wrote ${out.length} cases to ${path.relative(process.cwd(), target)}`);
