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
  const useGzip = shouldUseGzip(request, artifact);
  const r2Key = downloadCacheKey(artifact, useGzip ? 'gzip-file' : 'plain');
  const cached = await env.DOWNLOADS.get(r2Key);
  if (cached) return r2DownloadResponse(cached, artifact, request.method);

  const upstream = await fetchDownload(env, artifact);
  if (!upstream.ok || !upstream.body)
    throw new HttpError(`failed to download ${artifact.name}`, upstream.status || 502);

  if (artifact.zip_entry) {
    const zip = await upstream.arrayBuffer();
    const entry = await readZipEntry(zip, artifact.zip_entry);
    return cacheAndRespondBytes(request, env, ctx, artifact, entry, useGzip);
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
    const plainKey = downloadCacheKey(artifact, 'plain');
    const gzipKey = downloadCacheKey(artifact, 'gzip-file');
    const plainExists = Boolean(await env.DOWNLOADS.head(plainKey));
    const gzipExists = artifact.zip_entry ? Boolean(await env.DOWNLOADS.head(gzipKey)) : true;
    if (plainExists && gzipExists) return;

    const upstream = await fetchDownload(env, artifact);
    if (!upstream.ok || !upstream.body) {
      console.warn(`failed to prefetch ${artifact.name}: ${upstream.status}`);
      return;
    }

    if (artifact.zip_entry) {
      const zip = await upstream.arrayBuffer();
      const entry = await readZipEntry(zip, artifact.zip_entry);
      if (!plainExists) await putBytes(env, artifact, entry, 'application/octet-stream', 'plain');
      if (!gzipExists) await putGzipBytes(env, artifact, entry);
      return;
    }

    const contentType =
      upstream.headers.get('Content-Type') || artifact.artifact.content_type || 'application/zip';

    await env.DOWNLOADS.put(plainKey, upstream.body, {
      httpMetadata: {
        contentType,
        contentDisposition: `attachment; filename="${downloadFilename(artifact)}"`,
        cacheControl: `public, max-age=${cacheTtl(env)}`,
      },
      customMetadata: metadataFor(artifact),
    });
  });
}

export async function annotateGzipSizes(env: Env, body: unknown): Promise<unknown> {
  if (!body || typeof body !== 'object') return body;
  const clone = structuredClone(body);
  if (!clone || typeof clone !== 'object') return body;

  await Promise.all(
    manifestSections(clone).flatMap(items =>
      items.map(async item => {
        if (!isResolvedArtifact(item) || !item.gzip_url || !item.zip_entry) return;
        const object = await env.DOWNLOADS.head(downloadCacheKey(item, 'gzip-file'));
        if (object) item.gzip_size_in_bytes = object.size;
      }),
    ),
  );

  return clone;
}

type WaitUntilContext = {
  waitUntil(promise: Promise<unknown>): void;
};

export function downloadFilename(artifact: ResolvedArtifact): string {
  if (artifact.filename) return artifact.filename;
  if (artifact.source.type === 'release') return artifact.artifact.name;
  return `${artifact.name}.zip`;
}

async function cacheAndRespondBytes(
  request: Request,
  env: Env,
  ctx: WaitUntilContext,
  artifact: ResolvedArtifact,
  bytes: Uint8Array,
  useGzip: boolean,
): Promise<Response> {
  const body = new Uint8Array(bytes);
  const gzipBody = useGzip ? await gzipArrayBuffer(body) : undefined;
  const headers = downloadHeaders(
    artifact,
    useGzip ? 'application/gzip' : 'application/octet-stream',
    cacheTtl(env),
    useGzip ? 'gzip' : undefined,
    body.byteLength,
  );

  if (request.method !== 'HEAD') {
    ctx.waitUntil(
      Promise.all([
        putBytes(env, artifact, body, 'application/octet-stream', 'plain'),
        gzipBody
          ? putGzipObject(env, artifact, gzipBody, body.byteLength)
          : putGzipBytes(env, artifact, body),
      ]).catch(error => console.warn(`failed to cache ${artifact.name} in R2`, error)),
    );
  }

  if (useGzip) {
    if (!gzipBody) throw new HttpError(`failed to gzip ${artifact.name}`, 500);
    return new Response(request.method === 'HEAD' ? null : gzipBody, {
      status: 200,
      headers,
    });
  }

  return new Response(request.method === 'HEAD' ? null : body, { status: 200, headers });
}

async function putBytes(
  env: Env,
  artifact: ResolvedArtifact,
  body: ReadableStream | Uint8Array,
  contentType: string,
  variant: DownloadVariant,
): Promise<void> {
  await env.DOWNLOADS.put(downloadCacheKey(artifact, variant), body, {
    httpMetadata: {
      contentType,
      contentDisposition: `attachment; filename="${downloadFilename(artifact)}"`,
      cacheControl: `public, max-age=${cacheTtl(env)}`,
    },
    customMetadata: metadataFor(artifact),
  });
}

async function putGzipBytes(env: Env, artifact: ResolvedArtifact, body: Uint8Array): Promise<void> {
  await putGzipObject(env, artifact, await gzipArrayBuffer(body), body.byteLength);
}

async function putGzipObject(
  env: Env,
  artifact: ResolvedArtifact,
  body: ArrayBuffer,
  uncompressedSize: number,
): Promise<void> {
  await env.DOWNLOADS.put(downloadCacheKey(artifact, 'gzip-file'), body, {
    httpMetadata: {
      contentType: 'application/gzip',
      contentDisposition: `attachment; filename="${downloadFilename(artifact)}.gz"`,
      cacheControl: `public, max-age=${cacheTtl(env)}`,
    },
    customMetadata: {
      ...metadataFor(artifact),
      compression: 'gzip',
      uncompressed_size: String(uncompressedSize),
    },
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

function downloadHeaders(
  artifact: ResolvedArtifact,
  contentType: string,
  maxAge: number,
  encoding?: 'gzip',
  uncompressedSize?: number,
): Headers {
  const headers = new Headers();
  headers.set('Content-Type', contentType);
  headers.set(
    'Content-Disposition',
    `attachment; filename="${downloadFilename(artifact)}${encoding ? '.gz' : ''}"`,
  );
  headers.set('Cache-Control', `public, max-age=${maxAge}`);
  headers.set('X-Ant-Artifact', artifact.name);
  headers.set('X-Ant-Source', artifact.source.type);
  if (artifact.version) headers.set('X-Ant-Version', artifact.version);
  if (encoding) {
    headers.set('X-Ant-Compression', encoding);
  }
  if (uncompressedSize !== undefined) {
    headers.set('X-Ant-Uncompressed-Size', String(uncompressedSize));
  }
  headers.set('Access-Control-Allow-Origin', '*');
  return headers;
}

type DownloadVariant = 'plain' | 'gzip-file';

function downloadCacheKey(artifact: ResolvedArtifact, variant: DownloadVariant): string {
  return [
    'downloads',
    artifact.kind,
    artifact.source.type,
    String(artifact.artifact.id),
    variant,
    downloadFilename(artifact).replace(/[^A-Za-z0-9._-]/g, '_'),
  ].join('/');
}

function shouldUseGzip(request: Request, artifact: ResolvedArtifact): boolean {
  if (!artifact.zip_entry) return false;
  const value = new URL(request.url).searchParams.get('gzip');
  return value === '1' || value === 'true' || value === 'yes';
}

function byteStream(body: Uint8Array): ReadableStream<Uint8Array> {
  return new ReadableStream({
    start(controller) {
      controller.enqueue(body);
      controller.close();
    },
  });
}

function gzipBytes(body: Uint8Array): ReadableStream<Uint8Array> {
  return (byteStream(body) as ReadableStream<BufferSource>).pipeThrough(
    new CompressionStream('gzip'),
  ) as ReadableStream<Uint8Array>;
}

async function gzipArrayBuffer(body: Uint8Array): Promise<ArrayBuffer> {
  return new Response(gzipBytes(body)).arrayBuffer();
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

  const compression = object.customMetadata?.compression;
  if (compression === 'gzip') headers.set('X-Ant-Compression', 'gzip');
  const uncompressedSize = object.customMetadata?.uncompressed_size;
  if (uncompressedSize) headers.set('X-Ant-Uncompressed-Size', uncompressedSize);

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

function manifestSections(body: object): unknown[][] {
  const manifest = body as { sandbox?: unknown[]; kernel?: unknown[] };
  return [manifest.sandbox || [], manifest.kernel || []];
}

function isResolvedArtifact(value: unknown): value is ResolvedArtifact {
  return Boolean(
    value &&
      typeof value === 'object' &&
      (value as { available?: unknown }).available === true &&
      (value as { artifact?: unknown }).artifact &&
      (value as { source?: unknown }).source,
  );
}
