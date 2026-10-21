import { readSource } from 'upm/src/foreign-lock.ts';
import type { Node, Source } from 'upm/src/foreign-lock.ts';
import { LOCKFILE, writeLockfile } from 'upm/src/lock.ts';
import { readConfig } from 'upm/src/config.ts';
import { hosts } from 'upm/src/registry.ts';
import { declaredSpecs } from 'upm/src/resolve.ts';
import type { RootManifest } from 'upm/src/resolve.ts';

import fs from 'node:fs';
import path from 'node:path';

import { LOCKB, parseLockb } from './parse.ts';
import type { LockbPackage } from './parse.ts';

function installPath(pkg: LockbPackage): string {
  return pkg.parent ? `${pkg.parent}/node_modules/${pkg.name}` : pkg.name;
}

export function lockbSource(packages: LockbPackage[], manifest: RootManifest): Source {
  const byPath = new Map<string, LockbPackage>();
  for (const pkg of packages) byPath.set(installPath(pkg), pkg);

  const resolve = (from: LockbPackage, name: string): LockbPackage | undefined => {
    for (let dir: string | undefined = installPath(from); dir !== undefined; ) {
      const hit = byPath.get(`${dir}/node_modules/${name}`);
      if (hit) return hit;
      const up = dir.lastIndexOf('/node_modules/');
      dir = up < 0 ? undefined : dir.slice(0, up);
    }
    return byPath.get(name);
  };

  const nodes: Node[] = packages.map(pkg => {
    const node: Node = {
      name: pkg.name,
      version: pkg.version,
      resolved: pkg.resolved,
      integrity: pkg.integrity,
      dependencies: {},
      optionalDependencies: {},
      ...(pkg.bin && { bin: pkg.bin }),
      ...(pkg.hasBin && !pkg.bin && { hasBin: true }),
      ...(pkg.os && { os: pkg.os }),
      ...(pkg.cpu && { cpu: pkg.cpu }),
      ...(pkg.libc && { libc: pkg.libc }),
    };

    for (const dep of pkg.dependencies) {
      const target = resolve(pkg, packages[dep.index]!.name);
      if (!target) continue;
      const edges = dep.optional ? node.optionalDependencies : node.dependencies;
      edges[target.name] = target.version;
      if (!dep.peer) continue;
      (node.peerDependencies ??= {})[target.name] = dep.constraint || '*';
      (node.peers ??= {})[target.name] = dep.optional ? 'optional' : 'required';
    }
    return node;
  });

  const root: Record<string, string> = {};
  for (const pkg of packages) if (pkg.direct && !pkg.parent) root[pkg.name] = pkg.version;

  return { nodes, specs: declaredSpecs(manifest) ?? {}, root };
}

export async function migrateLockb(dir: string, log: (message: string) => void): Promise<boolean> {
  const lockb = path.join(dir, LOCKB);
  if (!fs.existsSync(lockb) || fs.existsSync(path.join(dir, LOCKFILE))) return false;

  let manifest: RootManifest;
  try {
    manifest = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')) as RootManifest;
  } catch {
    return false;
  }

  if ((manifest as { workspaces?: unknown }).workspaces) {
    log(`not migrating ${LOCKB}: workspaces are resolved again`);
    return false;
  }

  try {
    const packages = parseLockb(fs.readFileSync(lockb));
    const { registry, scopes } = readConfig(dir);
    const { lock } = readSource(LOCKB, lockbSource(packages, manifest), manifest, hosts(registry, scopes));
    await writeLockfile(dir, lock);
    log(`migrated ${LOCKB} to ${LOCKFILE} · ${Object.keys(lock.packages).length} pkgs`);
    return true;
  } catch (error) {
    log(`not migrating ${LOCKB}, resolving again: ${(error as Error).message}`);
    return false;
  }
}
