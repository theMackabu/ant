import { LOCKFILE } from 'upm/src/lock.ts';
import { STATE_FILE, TREE_LOCK } from 'upm/src/state.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec, positionals } from '../args.ts';
import { readManifest } from '../project.ts';
import { finish, migrate, upm } from '../session.ts';
import type { Session } from '../session.ts';

function forget(root: string): void {
  fs.rmSync(path.join(root, LOCKFILE), { force: true });
  fs.rmSync(path.join(root, 'node_modules', STATE_FILE), { force: true });
  fs.rmSync(path.join(root, 'node_modules', TREE_LOCK), { force: true });
}

export async function update(args: string[], session: Session): Promise<number> {
  const names = positionals(args);
  const flags = args.filter(arg => !names.includes(arg));
  const root = session.dir;
  await migrate(session);

  if (names.length === 0) {
    if (root) forget(root);
    return await finish(await upm(['install', ...flags], session), session);
  }

  const manifest = root ? readManifest(root) ?? {} : {};
  const isDev = (spec: string) => {
    const name = nameOfSpec(spec);
    return manifest.dependencies?.[name] === undefined && manifest.devDependencies?.[name] !== undefined;
  };
  const prod = names.filter(spec => !isDev(spec));
  const dev = names.filter(isDev);

  if (prod.length > 0) {
    const code = await upm(['add', ...prod, ...flags], session);
    if (code !== 0) return code;
  }
  if (dev.length > 0) {
    const code = await upm(['add', '--dev', ...dev, ...flags], session);
    if (code !== 0) return code;
  }
  return await finish(0, session);
}
