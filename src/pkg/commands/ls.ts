import path from 'node:path';

import { parseArgs } from '../args.ts';
import { globalDir } from '../paths.ts';
import { readLock, readManifest } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { c } from '../ui/ansi.ts';
import { out } from '../ui/output.ts';

export function ls(args: string[], session: Session): number {
  const { global } = parseArgs(args);
  const dir = global ? globalDir() : projectRoot(session);
  const manifest = readManifest(dir);
  const lock = readLock(dir);
  out(`${c.dim}${path.join(dir, 'node_modules')}${c.reset}`);

  if (!manifest) {
    out('\n  (no package.json)\n');
    return 0;
  }

  const names = Object.keys({ ...manifest.dependencies, ...(global ? {} : manifest.devDependencies) });
  if (names.length === 0) {
    out('\n  (no dependencies)\n');
    return 0;
  }

  const installed = names.filter(name => lock?.root.dependencies[name] !== undefined);
  const total = lock ? Object.keys(lock.packages).length : 0;
  if (installed.length === 0 || total === 0) {
    out('\n  (empty)\n');
    return 0;
  }

  out(` ${c.dim}(${total})${c.reset}\n`);
  installed.forEach((name, i) => {
    const prefix = i === installed.length - 1 ? '└──' : '├──';
    out(`${c.dim}${prefix}${c.reset} ${c.bold}${name}${c.reset}@${lock!.root.dependencies[name]}\n`);
  });
  return 0;
}
