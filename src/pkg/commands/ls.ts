import path from 'node:path';

import { isGlobal } from '../args.ts';
import { globalDir } from '../paths.ts';
import { readLock, readManifest } from '../project.ts';
import { projectRoot } from '../session.ts';
import type { Session } from '../session.ts';
import { color, note, out, plural } from '../ui/output.ts';

export function ls(args: string[], session: Session): number {
  const global = isGlobal(args);
  const dir = global ? globalDir() : projectRoot(session);
  const manifest = readManifest(dir);
  const lock = readLock(dir);
  const names = Object.keys({ ...manifest?.dependencies, ...(global ? {} : manifest?.devDependencies) });
  const installed = names.filter(name => lock?.root.dependencies[name] !== undefined);
  if (!manifest || installed.length === 0) {
    note(`nothing installed in ${path.join(dir, 'node_modules')}`);
    return 0;
  }

  const total = Object.keys(lock!.packages).length;
  out(`${color('gray', `${path.join(dir, 'node_modules')} · ${plural(total, 'pkg')}`)}\n`);
  for (const name of installed) out(`  ${color('cyan', name)}${color('gray', `@${lock!.root.dependencies[name]}`)}\n`);
  return 0;
}
