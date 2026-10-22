import { resolve } from 'upm/src/api.ts';
import { readConfig } from 'upm/src/config.ts';
import { registryBase } from 'upm/src/registry.ts';
import { parseSpec } from 'upm/src/spec.ts';

import fs from 'node:fs';
import path from 'node:path';

import { nameOfSpec } from './args.ts';
import type { SourcedSpec } from './args.ts';
import { registryOf } from './registry.ts';
import type { Source } from './settings.ts';
import { note } from './ui/output.ts';

const REGISTRY_TYPES = new Set(['range', 'version', 'tag']);
const SOURCE_NAMES: Record<Source, string> = { land: 'ants.land', npm: 'npm' };

function scopeOf(name: string): string | undefined {
  return name.startsWith('@') ? name.slice(0, name.indexOf('/')) : undefined;
}

function scopeRegistry(dir: string, scope: string): string | undefined {
  return readConfig(dir).scopes[scope];
}

function mapScope(dir: string, scope: string, registry: string): void {
  const npmrc = path.join(dir, '.npmrc');
  let text = '';
  try {
    text = fs.readFileSync(npmrc, 'utf8');
  } catch {}

  const prefix = text && !text.endsWith('\n') ? '\n' : '';
  fs.writeFileSync(npmrc, `${text}${prefix}${scope}:registry=${registry}\n`);
  note(`+ ${scope}:registry=${registry} in .npmrc`);
}

function isRegistrySpec(spec: string): boolean {
  try {
    const parsed = parseSpec(spec);
    return REGISTRY_TYPES.has(parsed.type) && parsed.fetchName === parsed.name;
  } catch {
    return false;
  }
}

async function tarballOn(spec: string, dir: string, registry: string): Promise<string | undefined> {
  try {
    const [manifest] = await resolve([spec], {
      dir,
      registry,
      log: () => {},
      experimental: { resolvePool: 0 },
    } as Parameters<typeof resolve>[1]);
    return manifest?.dist.tarball;
  } catch {
    return undefined;
  }
}

async function pinSpec(spec: string, source: Source, explicit: boolean, dir: string): Promise<string> {
  const registry = registryOf(source);
  const name = nameOfSpec(spec);
  const tarball = await tarballOn(spec, dir, registry);
  if (!tarball) {
    if (explicit) throw new Error(`${spec} was not found on ${SOURCE_NAMES[source]}`);
    return spec;
  }

  const scope = scopeOf(name);
  const mapped = scope ? scopeRegistry(dir, scope) : undefined;
  if (scope && (!mapped || registryBase(mapped) === registryBase(registry))) {
    if (!mapped) mapScope(dir, scope, registry);
    return spec;
  }

  note(`saving ${name} as its ${SOURCE_NAMES[source]} tarball url, so every install takes it from there`);
  return `${name}@${tarball}`;
}

export async function pinSpecs(specs: SourcedSpec[], dir: string): Promise<string[]> {
  const project = readConfig(dir).registry;
  const onLand = project === registryBase(registryOf('land'));
  const pinned: string[] = [];
  for (const { spec, source } of specs) {
    const target = source ?? (onLand ? 'land' : undefined);
    const needed = target && isRegistrySpec(spec) && !(target === 'npm' && project === registryBase(registryOf('npm')));
    pinned.push(needed ? await pinSpec(spec, target, source !== undefined, dir) : spec);
  }
  return pinned;
}
