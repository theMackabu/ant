import { readConfig } from 'upm/src/config.ts';
import { registryBase, tarballUrl } from 'upm/src/registry.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec } from './args.ts';
import { GROUPS, readLock, readManifest, writeJson } from './project.ts';
import { LAND_REGISTRY } from './registry.ts';
import { note } from './ui/output.ts';

function scopeOf(name: string): string | undefined {
  return name.startsWith('@') ? name.slice(0, name.indexOf('/')) : undefined;
}

function ensureScopeMapping(dir: string, scope: string): boolean {
  const npmrc = path.join(dir, '.npmrc');
  const key = `${scope}:registry`;
  let text = '';
  
  try {
    text = fs.readFileSync(npmrc, 'utf8');
  } catch {}

  const mapped = text.split(/\r?\n/).some(line => line.trim().toLowerCase().startsWith(`${key.toLowerCase()}=`));
  if (mapped) return false;

  const prefix = text && !text.endsWith('\n') ? '\n' : '';
  fs.writeFileSync(npmrc, `${text}${prefix}${key}=${LAND_REGISTRY}\n`);
  return true;
}

export function landCompatibility(dir: string, specs: string[]): boolean {
  const registry = readConfig(dir).registry;
  const land = registryBase(LAND_REGISTRY);
  const lock = readLock(dir);
  const manifest = readManifest(dir);
  if (!lock || !manifest) return false;

  const scopes = new Set<string>();
  const urls: string[] = [];

  for (const name of new Set(specs.map(nameOfSpec))) {
    const version = lock.root.dependencies[name];
    const entry = version === undefined ? undefined : lock.packages[`${name}@${version}`];
    if (!entry) continue;

    const resolved = entry.resolved ?? tarballUrl(registry, name, version!);
    if (!resolved.startsWith(`${land}/`)) continue;

    const scope = scopeOf(name);
    if (scope) {
      if (!scopes.has(scope) && ensureScopeMapping(dir, scope)) note(`+ ${scope}:registry=${LAND_REGISTRY} in .npmrc`);
      scopes.add(scope);
      continue;
    }

    for (const group of GROUPS) {
      const deps = manifest[group];
      if (!deps || !Object.hasOwn(deps, name)) continue;
      deps[name] = resolved;
      urls.push(name);
    }
  }

  if (urls.length === 0) return false;
  writeJson(path.join(dir, 'package.json'), manifest);
  note(`saved ${urls.join(', ')} as ants.land tarball urls, so other package managers can install them`);
  return true;
}
