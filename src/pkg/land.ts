import { resolve } from 'upm/src/api.ts';
import { readConfig } from 'upm/src/config.ts';
import { registryBase } from 'upm/src/registry.ts';
import { parseSpec } from 'upm/src/spec.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec } from './args.ts';
import { LAND_REGISTRY } from './registry.ts';
import { note } from './ui/output.ts';

function scopeOf(name: string): string | undefined {
  return name.startsWith('@') ? name.slice(0, name.indexOf('/')) : undefined;
}

function mapScope(dir: string, scope: string): void {
  const npmrc = path.join(dir, '.npmrc');
  const key = `${scope}:registry`;
  let text = '';

  try {
    text = fs.readFileSync(npmrc, 'utf8');
  } catch {}

  const mapped = text.split(/\r?\n/).some(line => line.trim().toLowerCase().startsWith(`${key.toLowerCase()}=`));
  if (mapped) return;

  const prefix = text && !text.endsWith('\n') ? '\n' : '';
  fs.writeFileSync(npmrc, `${text}${prefix}${key}=${LAND_REGISTRY}\n`);
  note(`+ ${key}=${LAND_REGISTRY} in .npmrc`);
}

const REGISTRY_TYPES = new Set(['range', 'version', 'tag']);

function isRegistrySpec(spec: string): boolean {
  try {
    const parsed = parseSpec(spec);
    return REGISTRY_TYPES.has(parsed.type) && parsed.fetchName === parsed.name;
  } catch {
    return false;
  }
}

async function onLand(spec: string, dir: string): Promise<string | undefined> {
  try {
    const [manifest] = await resolve([spec], {
      dir,
      registry: LAND_REGISTRY,
      log: () => {},
      experimental: { resolvePool: 0 },
    } as Parameters<typeof resolve>[1]);
    return manifest?.dist.tarball;
  } catch {
    return undefined;
  }
}

export async function pinLandSpecs(specs: string[], dir: string): Promise<string[]> {
  if (readConfig(dir).registry !== registryBase(LAND_REGISTRY)) return specs;
  const pinned: string[] = [];
  for (const spec of specs) {
    const name = nameOfSpec(spec);
    const tarball = isRegistrySpec(spec) ? await onLand(spec, dir) : undefined;
    if (!tarball) {
      pinned.push(spec);
      continue;
    }
    const scope = scopeOf(name);
    if (scope) {
      mapScope(dir, scope);
      pinned.push(spec);
      continue;
    }
    note(`saving ${name} as its ants.land tarball url, so other package managers can install it`);
    pinned.push(`${name}@${tarball}`);
  }
  return pinned;
}
