import type { Env } from './types';

export const BUILD_WORKFLOW = 'build.yml';
export const MUSL_SANDBOX_WORKFLOW = 'build-musl-sandboxes.yml';

export const DEFAULT_BRANCH = 'master';
export const DEFAULT_REPOSITORY = 'theMackabu/ant';

export const DEFAULT_ACTION_REPOSITORIES = ['sf-tools/ant', 'theMackabu/ant'];
export const DEFAULT_RELEASE_REPOSITORY = 'theMackabu/ant';

export const DEFAULT_CACHE_TTL_SECONDS = 300;
export const DEFAULT_MANIFEST_REFRESH_SECONDS = 21600;

export type RequestOptions = {
  branch?: string;
  runId?: number;
  revision?: string;
};

export function canDownloadActionsArtifacts(env: Env): boolean {
  return Boolean(env.GITHUB_TOKEN);
}

export function repository(env: Env): string {
  return env.GITHUB_REPOSITORY || DEFAULT_REPOSITORY;
}

export function releaseRepository(env: Env): string {
  return env.GITHUB_RELEASE_REPOSITORY || DEFAULT_RELEASE_REPOSITORY;
}

export function actionRepositories(env: Env): string[] {
  const configured = env.GITHUB_ACTION_REPOSITORIES;
  if (!configured) return DEFAULT_ACTION_REPOSITORIES;

  const repos = configured
    .split(',')
    .map(repo => repo.trim())
    .filter(Boolean);

  return repos.length ? [...new Set(repos)] : DEFAULT_ACTION_REPOSITORIES;
}

export function branch(env: Env, options?: RequestOptions): string {
  return options?.branch || env.GITHUB_BRANCH || DEFAULT_BRANCH;
}

export function actionsRunId(env: Env, options?: RequestOptions): number | undefined {
  if (options?.runId) return options.runId;
  const raw = env.GITHUB_RUN_ID;
  if (!raw) return undefined;
  const parsed = Number(raw);
  return Number.isSafeInteger(parsed) && parsed > 0 ? parsed : undefined;
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
