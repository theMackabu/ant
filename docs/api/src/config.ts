import type { Env } from './types';

export const BUILD_WORKFLOW = 'build.yml';
export const MUSL_SANDBOX_WORKFLOW = 'build-musl-sandboxes.yml';

export const DEFAULT_BRANCH = 'master';
export const GITHUB_REPOSITORY = 'theMackabu/ant';

export const DEFAULT_CACHE_TTL_SECONDS = 300;

export const CHANNELS = ['stable', 'canary'] as const;
export type Channel = (typeof CHANNELS)[number];
export const DEFAULT_CHANNEL: Channel = 'stable';

export function manifestKey(channel: Channel = DEFAULT_CHANNEL): string {
  const name = channel === DEFAULT_CHANNEL ? 'latest' : channel;
  return `manifests/${GITHUB_REPOSITORY}/${name}.json`;
}

export function manifestKeys(): string[] {
  return CHANNELS.map(channel => manifestKey(channel));
}

export type RequestOptions = {
  channel?: Channel;
  branch?: string;
  runId?: number;
  revision?: string;
  version?: string;
};

export function canDownloadActionsArtifacts(env: Env): boolean {
  return Boolean(env.GITHUB_TOKEN);
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
