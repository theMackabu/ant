import { branch, manifestRefreshSeconds, repository } from './config';
import { prefetchArtifacts } from './downloads';
import type { Env, ResolvedArtifact } from './types';

type WaitUntilContext = {
  waitUntil(promise: Promise<unknown>): void;
};

type ManifestProducer = () => Promise<unknown>;

export async function cachedManifest(
  env: Env,
  ctx: WaitUntilContext,
  producer: ManifestProducer,
): Promise<Response> {
  const key = manifestKey(env);
  const cached = await env.DOWNLOADS.get(key);

  if (cached) {
    const headers = jsonHeaders();
    cached.writeHttpMetadata(headers);
    headers.set('X-Ant-Manifest-Cache', 'hit');
    headers.set('X-Ant-Manifest-Cached-At', cached.customMetadata?.cached_at || '');

    if (shouldRefreshManifest(env, cached)) {
      headers.set('X-Ant-Manifest-Refresh', 'background');
      ctx.waitUntil(refreshManifest(env, key, producer));
    }

    return new Response(cached.body, { status: 200, headers });
  }

  const body = await producer();
  const response = manifestResponse(body, 'miss');
  ctx.waitUntil(storeManifestAndPrefetch(env, key, body));
  return response;
}

export async function forceRefreshManifest(
  env: Env,
  ctx: WaitUntilContext,
  producer: ManifestProducer,
): Promise<Response> {
  const key = manifestKey(env);
  const body = await producer();
  await storeManifest(env, key, body);
  ctx.waitUntil(prefetchArtifacts(env, manifestArtifacts(body)));

  return Response.json(
    {
      ok: true,
      refreshed: true,
      cache_key: key,
      artifact_count: manifestArtifacts(body).length,
    },
    { headers: jsonHeaders() },
  );
}

async function refreshManifest(env: Env, key: string, producer: ManifestProducer): Promise<void> {
  try {
    await storeManifestAndPrefetch(env, key, await producer());
  } catch (error) {
    console.warn(`failed to refresh manifest cache ${key}`, error);
  }
}

async function storeManifestAndPrefetch(env: Env, key: string, body: unknown): Promise<void> {
  await storeManifest(env, key, body);
  await prefetchArtifacts(env, manifestArtifacts(body));
}

async function storeManifest(env: Env, key: string, body: unknown): Promise<void> {
  await env.DOWNLOADS.put(key, JSON.stringify(body, null, 2) + '\n', {
    httpMetadata: {
      contentType: 'application/json; charset=utf-8',
      cacheControl: 'public, max-age=60',
    },
    customMetadata: {
      repository: repository(env),
      branch: branch(env),
      cached_at: new Date().toISOString(),
    },
  });
}

function manifestResponse(body: unknown, cacheState: string): Response {
  const headers = jsonHeaders();
  headers.set('X-Ant-Manifest-Cache', cacheState);
  return Response.json(body, { headers });
}

function shouldRefreshManifest(env: Env, object: R2Object): boolean {
  const cachedAt = Date.parse(object.customMetadata?.cached_at || '');
  if (!Number.isFinite(cachedAt)) return true;
  return Date.now() - cachedAt > manifestRefreshSeconds(env) * 1000;
}

function manifestKey(env: Env): string {
  return `manifests/latest/${repository(env)}/${branch(env)}.json`;
}

function jsonHeaders(): Headers {
  return new Headers({
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'public, max-age=60',
    'Access-Control-Allow-Origin': '*',
    'X-Content-Type-Options': 'nosniff',
  });
}

function manifestArtifacts(body: unknown): ResolvedArtifact[] {
  if (!body || typeof body !== 'object') return [];
  const manifest = body as {
    ant?: unknown[];
    sandbox?: unknown[];
    kernel?: unknown[];
  };

  return [...(manifest.ant || []), ...(manifest.sandbox || []), ...(manifest.kernel || [])].filter(
    isAvailableArtifact,
  );
}

function isAvailableArtifact(value: unknown): value is ResolvedArtifact {
  return Boolean(
    value &&
      typeof value === 'object' &&
      (value as { available?: unknown }).available === true &&
      (value as { artifact?: unknown }).artifact &&
      (value as { source?: unknown }).source,
  );
}
