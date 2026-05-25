import { HttpError } from './errors';
import { readZipEntry } from './zip';
import { fetchDownload } from './resolver';
import type { Env, ResolvedArtifact } from './types';
import { DEFAULT_CACHE_TTL_SECONDS, cacheTtl } from './config';

export async function downloadArtifact(
  request: Request,
  env: Env,
  ctx: WaitUntilContext,
  artifact: ResolvedArtifact,
): Promise<Response> {
  const r2Key = downloadCacheKey(artifact);
  const cached = await env.DOWNLOADS.get(r2Key);
  if (cached) return r2DownloadResponse(cached, artifact, request.method);

  const upstream = await fetchDownload(env, artifact);
  if (!upstream.ok || !upstream.body)
    throw new HttpError(`failed to download ${artifact.name}`, upstream.status || 502);

  if (artifact.zip_entry) {
    const zip = await upstream.arrayBuffer();
    const entry = await readZipEntry(zip, artifact.zip_entry);
    return cacheAndRespondBytes(request, env, ctx, artifact, entry);
  }

  const headers = new Headers();
  headers.set(
    'Content-Type',
    upstream.headers.get('Content-Type') || artifact.artifact.content_type || 'application/zip',
  );

  headers.set('Content-Disposition', `attachment; filename="${downloadFilename(artifact)}"`);
  headers.set('Cache-Control', `public, max-age=${cacheTtl(env)}`);
  headers.set('X-Ant-Artifact', artifact.name);
  headers.set('X-Ant-Source', artifact.source.type);

  if (artifact.version) headers.set('X-Ant-Version', artifact.version);
  headers.set('Access-Control-Allow-Origin', '*');

  if (request.method === 'HEAD') return new Response(null, { status: 200, headers });

  const [clientBody, cacheBody] = upstream.body.tee();
  ctx.waitUntil(
    env.DOWNLOADS.put(r2Key, cacheBody, {
      httpMetadata: {
        contentType: headers.get('Content-Type') || undefined,
        contentDisposition: headers.get('Content-Disposition') || undefined,
        cacheControl: headers.get('Cache-Control') || undefined,
      },
      customMetadata: {
        artifact: artifact.name,
        kind: artifact.kind,
        source: artifact.source.type,
        version: artifact.version || '',
      },
    }).catch(error => console.warn(`failed to cache ${r2Key} in R2`, error)),
  );

  return new Response(clientBody, { status: 200, headers });
}

export async function prefetchArtifacts(env: Env, artifacts: ResolvedArtifact[]): Promise<void> {
  await mapLimit(artifacts, 2, async artifact => {
    const key = downloadCacheKey(artifact);
    if (await env.DOWNLOADS.head(key)) return;

    const upstream = await fetchDownload(env, artifact);
    if (!upstream.ok || !upstream.body) {
      console.warn(`failed to prefetch ${artifact.name}: ${upstream.status}`);
      return;
    }

    if (artifact.zip_entry) {
      const zip = await upstream.arrayBuffer();
      const entry = await readZipEntry(zip, artifact.zip_entry);
      await putBytes(env, artifact, entry, 'application/octet-stream');
      return;
    }

    const contentType =
      upstream.headers.get('Content-Type') || artifact.artifact.content_type || 'application/zip';

    await env.DOWNLOADS.put(key, upstream.body, {
      httpMetadata: {
        contentType,
        contentDisposition: `attachment; filename="${downloadFilename(artifact)}"`,
        cacheControl: `public, max-age=${cacheTtl(env)}`,
      },
      customMetadata: metadataFor(artifact),
    });
  });
}

type WaitUntilContext = {
  waitUntil(promise: Promise<unknown>): void;
};

export function downloadFilename(artifact: ResolvedArtifact): string {
  if (artifact.filename) return artifact.filename;
  if (artifact.source.type === 'release') return artifact.artifact.name;
  return `${artifact.name}.zip`;
}

function cacheAndRespondBytes(
  request: Request,
  env: Env,
  ctx: WaitUntilContext,
  artifact: ResolvedArtifact,
  bytes: Uint8Array,
): Response {
  const headers = downloadHeaders(artifact, 'application/octet-stream', cacheTtl(env));
  const body = new Uint8Array(bytes);

  if (request.method !== 'HEAD')
    ctx.waitUntil(
      putBytes(env, artifact, body, 'application/octet-stream').catch(error =>
        console.warn(`failed to cache ${downloadCacheKey(artifact)} in R2`, error),
      ),
    );

  return new Response(request.method === 'HEAD' ? null : body, { status: 200, headers });
}

async function putBytes(
  env: Env,
  artifact: ResolvedArtifact,
  body: ReadableStream | Uint8Array,
  contentType: string,
): Promise<void> {
  await env.DOWNLOADS.put(downloadCacheKey(artifact), body, {
    httpMetadata: {
      contentType,
      contentDisposition: `attachment; filename="${downloadFilename(artifact)}"`,
      cacheControl: `public, max-age=${cacheTtl(env)}`,
    },
    customMetadata: metadataFor(artifact),
  });
}

function metadataFor(artifact: ResolvedArtifact): Record<string, string> {
  return {
    artifact: artifact.name,
    kind: artifact.kind,
    source: artifact.source.type,
    version: artifact.version || '',
    zip_entry: artifact.zip_entry || '',
  };
}

async function mapLimit<T>(
  items: T[],
  limit: number,
  worker: (item: T) => Promise<void>,
): Promise<void> {
  let next = 0;
  const workers = Array.from({ length: Math.min(limit, items.length) }, async () => {
    while (next < items.length) {
      const item = items[next++];
      try {
        await worker(item);
      } catch (error) {
        console.warn('prefetch failed', error);
      }
    }
  });
  await Promise.all(workers);
}

function downloadHeaders(artifact: ResolvedArtifact, contentType: string, maxAge: number): Headers {
  const headers = new Headers();
  headers.set('Content-Type', contentType);
  headers.set('Content-Disposition', `attachment; filename="${downloadFilename(artifact)}"`);
  headers.set('Cache-Control', `public, max-age=${maxAge}`);
  headers.set('X-Ant-Artifact', artifact.name);
  headers.set('X-Ant-Source', artifact.source.type);
  if (artifact.version) headers.set('X-Ant-Version', artifact.version);
  headers.set('Access-Control-Allow-Origin', '*');
  return headers;
}

function downloadCacheKey(artifact: ResolvedArtifact): string {
  return [
    'downloads',
    artifact.kind,
    artifact.source.type,
    String(artifact.artifact.id),
    downloadFilename(artifact).replace(/[^A-Za-z0-9._-]/g, '_'),
  ].join('/');
}

function r2DownloadResponse(
  object: R2ObjectBody,
  artifact: ResolvedArtifact,
  method: string,
): Response {
  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set('Access-Control-Allow-Origin', '*');
  headers.set('ETag', object.httpEtag);
  headers.set('X-Ant-Artifact', object.customMetadata?.artifact || artifact.name);
  headers.set('X-Ant-Source', object.customMetadata?.source || artifact.source.type);

  const version = object.customMetadata?.version || artifact.version;
  if (version) headers.set('X-Ant-Version', version);
  if (!headers.has('Cache-Control')) {
    headers.set('Cache-Control', `public, max-age=${DEFAULT_CACHE_TTL_SECONDS}`);
  }
  if (!headers.has('Content-Disposition')) {
    headers.set('Content-Disposition', `attachment; filename="${downloadFilename(artifact)}"`);
  }

  return new Response(method === 'HEAD' ? null : object.body, {
    status: 200,
    headers,
  });
}
