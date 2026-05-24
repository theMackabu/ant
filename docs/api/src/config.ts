import type { Env } from './types';

export const BUILD_WORKFLOW = 'build.yml';
export const MUSL_SANDBOX_WORKFLOW = 'build-musl-sandboxes.yml';

export const DEFAULT_BRANCH = 'master';
export const DEFAULT_REPOSITORY = 'themackabu/ant';

export const DEFAULT_CACHE_TTL_SECONDS = 300;
export const DEFAULT_MANIFEST_REFRESH_SECONDS = 21600;

export function canDownloadActionsArtifacts(env: Env): boolean {
  return Boolean(env.GITHUB_TOKEN);
}

export function repository(env: Env): string {
  return env.GITHUB_REPOSITORY || DEFAULT_REPOSITORY;
}

export function branch(env: Env): string {
  return env.GITHUB_BRANCH || DEFAULT_BRANCH;
}

export function withBranch(env: Env, value: string): Env {
  return {
    ...env,
    GITHUB_BRANCH: value,
  };
}

export function cacheTtl(env: Env): number {
  const parsed = Number(env.CACHE_TTL_SECONDS || DEFAULT_CACHE_TTL_SECONDS);
  return Number.isFinite(parsed) && parsed > 0 ? Math.floor(parsed) : DEFAULT_CACHE_TTL_SECONDS;
}

export function manifestRefreshSeconds(env: Env): number {
  const parsed = Number(env.MANIFEST_REFRESH_SECONDS || DEFAULT_MANIFEST_REFRESH_SECONDS);
  return Number.isFinite(parsed) && parsed > 0
    ? Math.floor(parsed)
    : DEFAULT_MANIFEST_REFRESH_SECONDS;
}
