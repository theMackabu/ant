import path from 'node:path';

import type { PkgConfig, Source } from './settings.ts';

export const LAND_REGISTRY = 'https://npm.ants.land';
export const NPM_REGISTRY = 'https://registry.npmjs.org';

export function registryOf(source: Source): string {
  return source === 'land' ? LAND_REGISTRY : NPM_REGISTRY;
}

export function applyRegistry(config: PkgConfig, prefixed?: Source): void {
  const env = process.env;
  if (prefixed) {
    env.UPM_DEFAULT_REGISTRY = registryOf(prefixed);
    delete env.UPM_FALLBACK_REGISTRY;
    return;
  }
  if (env.UPM_DEFAULT_REGISTRY) return;
  env.UPM_DEFAULT_REGISTRY = registryOf(config.defaultRegistry);
  if (config.defaultRegistry === 'land' && config.missingPackageFallback === 'npm') {
    env.UPM_FALLBACK_REGISTRY ??= NPM_REGISTRY;
  }
}

export function applyStore(config: PkgConfig, projectDir: string | undefined): void {
  if (config.store !== 'project' || !projectDir || process.env.UPM_STORE) return;
  process.env.UPM_STORE = path.join(projectDir, 'node_modules', '.ant-store');
}

export function registriesFor(config: PkgConfig): string[] {
  const primary = process.env.UPM_DEFAULT_REGISTRY || registryOf(config.defaultRegistry);
  const fallback = process.env.UPM_FALLBACK_REGISTRY;
  if (fallback) return [primary, fallback];
  if (primary !== NPM_REGISTRY && config.missingPackageFallback === 'npm') return [primary, NPM_REGISTRY];
  return [primary];
}
