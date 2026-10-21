import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import type { Session } from '../session.ts';
import { fail, note, summary, usage } from '../ui/output.ts';
import { exec } from './exec.ts';

function createPackage(template: string): string {
  if (!template.startsWith('@')) return `create-${template}`;
  const [scope, rest] = template.split('/');
  return rest ? `${scope}/create-${rest}` : `${scope}/create`;
}

export async function create(args: string[], session: Session): Promise<number> {
  const [template, ...rest] = args;
  if (!template) return usage('create needs a template or a GitHub repository');

  const github = template.includes('/') && !template.startsWith('@');
  if (!github) return await exec([createPackage(template), ...rest], session);

  const dest = rest.find(arg => !arg.startsWith('-')) ?? template.replace(/\.git$/, '').split('/').pop()!;
  if (fs.existsSync(dest)) {
    fail(`${dest} already exists`);
    return 1;
  }

  const url = template.startsWith('https://') || template.startsWith('git@') ? template : `https://github.com/${template}.git`;
  const clone = spawnSync('git', ['clone', '--quiet', '--depth', '1', url, dest], { stdio: 'inherit' });
  if (clone.status !== 0) {
    fail(`could not clone ${url}`);
    return 1;
  }
  fs.rmSync(path.join(dest, '.git'), { recursive: true, force: true });

  summary('created', [dest, `from ${template}`]);
  if (fs.existsSync(path.join(dest, 'package.json'))) note(`next: cd ${dest} && ant install`);
  return 0;
}
