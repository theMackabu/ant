import path from 'node:path';

import type { PkgConfig, Source } from './settings.ts';

export const LAND_REGISTRY = 'https://npm.ants.land';
const NPM_REGISTRY = 'https://registry.npmjs.org';

const registryOf = (source: Source) => (source === 'land' ? LAND_REGISTRY : NPM_REGISTRY);

export function useDefaultRegistry(config: PkgConfig): void {
  const env = process.env;
  if (env.UPM_DEFAULT_REGISTRY) return;
  env.UPM_DEFAULT_REGISTRY = registryOf(config.defaultRegistry);
  if (config.defaultRegistry === 'land' && config.missingPackageFallback === 'npm') {
    env.UPM_FALLBACK_REGISTRY ??= NPM_REGISTRY;
  }
}

export function useSource(source: Source): void {
  process.env.UPM_DEFAULT_REGISTRY = registryOf(source);
  delete process.env.UPM_FALLBACK_REGISTRY;
}

export function useProjectStore(config: PkgConfig, projectDir: string | undefined): boolean {
  if (config.store !== 'project' || !projectDir || process.env.UPM_STORE) return false;
  process.env.UPM_STORE = path.join(projectDir, 'node_modules', '.ant-store');
  return true;
}
