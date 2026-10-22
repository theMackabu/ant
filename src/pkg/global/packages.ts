import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec } from '../args.ts';
import type { SourcedSpec } from '../args.ts';
import { pinSpecs } from '../land.ts';
import { binDir, globalDir } from '../paths.ts';
import { readManifest, writeJson } from '../project.ts';
import { upm } from '../session.ts';
import type { Session } from '../session.ts';
import { fail, note } from '../ui/output.ts';
import { binsOf, linkBin, unlinkBin } from './bins.ts';

function openGlobal(session: Session): string {
  const dir = globalDir();
  const manifest = path.join(dir, 'package.json');
  if (!fs.existsSync(manifest)) writeJson(manifest, { dependencies: {} });
  if (session.projectStore) delete process.env.UPM_STORE;
  return dir;
}

export async function addGlobal(sourced: SourcedSpec[], flags: string[], session: Session): Promise<number> {
  const dir = openGlobal(session);
  const specs = await pinSpecs(sourced, dir);
  const before = readManifest(dir)?.dependencies ?? {};
  const code = await upm(['add', ...specs, ...flags, '--dir', dir], session);
  if (code !== 0) return code;

  const after = readManifest(dir)?.dependencies ?? {};
  const named = new Set(specs.map(nameOfSpec));
  const added = Object.keys(after).filter(name => named.has(name) || before[name] !== after[name]);

  const bins = binDir();
  fs.mkdirSync(bins, { recursive: true });
  const linked: string[] = [];
  for (const name of added) {
    const pkgDir = path.join(dir, 'node_modules', name);
    for (const [bin, file] of Object.entries(binsOf(pkgDir, name))) {
      if (linkBin(path.join(pkgDir, file), path.join(bins, bin), dir)) linked.push(bin);
    }
  }
  if (linked.length > 0) note(`linked ${linked.join(', ')} into ${bins}`);
  return 0;
}

export async function removeGlobal(names: string[], flags: string[], session: Session): Promise<number> {
  const dir = openGlobal(session);
  const deps = readManifest(dir)?.dependencies ?? {};
  const missing = names.filter(name => !Object.hasOwn(deps, name));
  if (missing.length > 0) {
    fail(`not installed globally: ${missing.join(', ')}`);
    return 1;
  }

  const bins = binDir();
  const unlinked: string[] = [];
  for (const name of names) {
    const pkgDir = path.join(dir, 'node_modules', name);
    for (const bin of Object.keys(binsOf(pkgDir, name))) {
      if (unlinkBin(path.join(bins, bin), pkgDir)) unlinked.push(bin);
    }
  }

  const code = await upm(['remove', ...names, ...flags, '--dir', dir], session);
  if (code === 0 && unlinked.length > 0) note(`unlinked ${unlinked.join(', ')} from ${bins}`);
  return code;
}
