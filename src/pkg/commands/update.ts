import { LOCKFILE } from 'upm/src/lock.ts';
import { TREE_LOCK } from 'upm/src/state.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec, parseArgs } from '../args.ts';
import type { Spec } from '../args.ts';
import { pinSpecs } from '../land.ts';
import { afterInstall } from '../lifecycle/report.ts';
import { migrateLockb } from '../lockb/migrate.ts';
import { GROUPS, readManifest } from '../project.ts';
import { sourceOfTarball } from '../registry.ts';
import { projectRoot, upm } from '../session.ts';
import type { Session } from '../session.ts';
import { fail } from '../ui/output.ts';

type Group = (typeof GROUPS)[number];

const GROUP_FLAGS: Record<Group, string[]> = {
  dependencies: [],
  devDependencies: ['--dev'],
  optionalDependencies: ['--optional'],
};

async function refreshAll(flags: string[], session: Session): Promise<number> {
  const root = session.dir;
  if (!root) {
    const code = await upm(['install', ...flags], session);
    return code !== 0 ? code : await afterInstall(session, flags);
  }

  const lockFile = path.join(root, LOCKFILE);
  const saved = fs.existsSync(lockFile) ? fs.readFileSync(lockFile) : undefined;
  fs.rmSync(lockFile, { force: true });
  fs.rmSync(path.join(root, 'node_modules', TREE_LOCK), { force: true });

  const code = await upm(['install', ...flags], session);
  const written = fs.existsSync(lockFile);
  if (code === 0 && written) return await afterInstall(session, flags);
  if (saved && !written) fs.writeFileSync(lockFile, saved);
  if (code !== 0) return code;
  return fail(`${LOCKFILE} was not rewritten: another package manager's lockfile was read instead`);
}

export async function update(args: string[], session: Session): Promise<number> {
  const { specs, flags } = parseArgs(args);
  await migrateLockb(session);
  if (specs.length === 0) return await refreshAll(flags, session);

  const manifest = (session.dir && readManifest(session.dir)) || {};
  const groups = new Map<Group, Spec[]>();
  for (const { spec, source } of specs) {
    const name = nameOfSpec(spec);
    const group = GROUPS.find(g => manifest[g]?.[name] !== undefined);
    if (!group) return fail(`${name} is not a dependency in package.json`);

    const declared = String(manifest[group]![name]);
    const pinned = spec === name ? sourceOfTarball(declared) : undefined;
    const wanted = spec !== name ? spec : pinned ? name : `${name}@${declared}`;
    groups.set(group, [...(groups.get(group) ?? []), { spec: wanted, source: source ?? pinned }]);
  }

  for (const [group, wanted] of groups) {
    const pinned = await pinSpecs(wanted, projectRoot(session));
    const code = await upm(['add', ...GROUP_FLAGS[group], ...pinned, ...flags], session);
    if (code !== 0) return code;
  }
  return await afterInstall(session, flags);
}
