import path from 'node:path';

import type { PkgConfig, Source } from './settings.ts';

export const LAND_REGISTRY = 'https://npm.ants.land';
const NPM_REGISTRY = 'https://registry.npmjs.org';

export const registryOf = (source: Source) => (source === 'land' ? LAND_REGISTRY : NPM_REGISTRY);

function useRegistry(source: Source, config: PkgConfig): void {
  const env = process.env;
  env.UPM_DEFAULT_REGISTRY = registryOf(source);
  if (source === 'land' && config.missingPackageFallback === 'npm') env.UPM_FALLBACK_REGISTRY = NPM_REGISTRY;
  else delete env.UPM_FALLBACK_REGISTRY;
}

export function useDefaultRegistry(config: PkgConfig): void {
  if (!process.env.UPM_DEFAULT_REGISTRY) useRegistry(config.defaultRegistry, config);
}

export function useSource(source: Source, config: PkgConfig): void {
  useRegistry(source, config);
}

export function useProjectStore(config: PkgConfig, projectDir: string | undefined): boolean {
  if (config.store !== 'project' || !projectDir || process.env.UPM_STORE) return false;
  process.env.UPM_STORE = path.join(projectDir, 'node_modules', '.ant-store');
  return true;
}
