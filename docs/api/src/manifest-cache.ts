import { branch, repository } from './config';
import type { RequestOptions } from './config';
import { annotateGzipSizes, downloadCacheKeys, prefetchArtifacts } from './downloads';
import type { Env, ResolvedArtifact } from './types';

type WaitUntilContext = {
  waitUntil(promise: Promise<unknown>): void;
};

type ManifestProducer = () => Promise<unknown>;

export async function cachedManifest(
  env: Env,
  options: RequestOptions,
  ctx: WaitUntilContext,
  producer: ManifestProducer,
): Promise<Response> {
  const key = manifestKey(env, options);
  const cached = await env.DOWNLOADS.get(key);

  if (cached) {
    const headers = jsonHeaders();
    cached.writeHttpMetadata(headers);
    headers.set('X-Ant-Manifest-Cache', 'hit');
    headers.set('X-Ant-Manifest-Cached-At', cached.customMetadata?.cached_at || '');

    return new Response(
      JSON.stringify(await annotateGzipSizes(env, await cached.json()), null, 2) + '\n',
      {
        status: 200,
        headers,
      },
    );
  }

  const body = await producer();
  const response = manifestResponse(body, 'miss');
  ctx.waitUntil(storeManifestAndPrefetch(env, options, key, body));
  return response;
}

export async function forceRefreshManifest(
  env: Env,
  options: RequestOptions,
  ctx: WaitUntilContext,
  producer: ManifestProducer,
): Promise<Response> {
  const key = manifestKey(env, options);
  const body = await producer();
  await prefetchArtifacts(env, manifestArtifacts(body));
  const annotated = await annotateGzipSizes(env, body);
  await storeManifest(env, options, key, annotated);
  await pruneR2ToLatest(env, key, annotated);

  return Response.json(
    {
      ok: true,
      refreshed: true,
      cache_key: key,
      artifact_count: manifestArtifacts(annotated).length,
    },
    { headers: jsonHeaders() },
  );
}

async function refreshManifest(env: Env, options: RequestOptions, key: string, producer: ManifestProducer): Promise<void> {
  try {
    await storeManifestAndPrefetch(env, options, key, await producer());
  } catch (error) {
    console.warn(`failed to refresh manifest cache ${key}`, error);
  }
}

async function storeManifestAndPrefetch(env: Env, options: RequestOptions, key: string, body: unknown): Promise<void> {
  await prefetchArtifacts(env, manifestArtifacts(body));
  const annotated = await annotateGzipSizes(env, body);
  await storeManifest(env, options, key, annotated);
  await pruneR2ToLatest(env, key, annotated);
}

async function storeManifest(env: Env, options: RequestOptions, key: string, body: unknown): Promise<void> {
  await env.DOWNLOADS.put(key, JSON.stringify(body, null, 2) + '\n', {
    httpMetadata: {
      contentType: 'application/json; charset=utf-8',
      cacheControl: 'public, max-age=60',
    },
    customMetadata: {
      repository: repository(env),
      branch: branch(env, options),
      cached_at: new Date().toISOString(),
    },
  });
}

function manifestResponse(body: unknown, cacheState: string): Response {
  const headers = jsonHeaders();
  headers.set('X-Ant-Manifest-Cache', cacheState);
  return Response.json(body, { headers });
}

function manifestKey(env: Env, options: RequestOptions): string {
  return `manifests/latest/${repository(env)}/${branch(env, options)}.json`;
}

async function pruneR2ToLatest(env: Env, manifestKey: string, body: unknown): Promise<void> {
  try {
    const keep = new Set<string>([manifestKey]);
    for (const artifact of manifestArtifacts(body)) {
      for (const key of downloadCacheKeys(artifact)) keep.add(key);
    }

    await prunePrefix(env, 'manifests/latest/', keep);
    await prunePrefix(env, 'downloads/', keep);
  } catch (error) {
    console.warn('failed to prune R2 cache', error);
  }
}

async function prunePrefix(env: Env, prefix: string, keep: Set<string>): Promise<void> {
  const stale: string[] = [];
  let cursor: string | undefined;

  do {
    const listed = await env.DOWNLOADS.list({ prefix, cursor });
    for (const object of listed.objects) {
      if (!keep.has(object.key)) stale.push(object.key);
    }
    cursor = listed.truncated ? listed.cursor : undefined;
  } while (cursor);

  for (let i = 0; i < stale.length; i += 1000) {
    await env.DOWNLOADS.delete(stale.slice(i, i + 1000));
  }
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
