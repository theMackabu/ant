import { cacheTtl } from './config';
import type { Env } from './types';

export async function cachedJson(
  request: Request,
  ctx: WaitUntilContext,
  env: Env,
  key: string,
  producer: () => Promise<unknown>,
): Promise<Response> {
  const cache = defaultCache();
  const cacheKey = new Request(`https://ant-api-cache.local/${encodeURIComponent(key)}`, {
    method: 'GET',
  });
  const cached = await cache.match(cacheKey);
  if (cached) return addCors(cached);

  const body = await producer();
  const response = Response.json(body, {
    headers: {
      'Cache-Control': `public, max-age=${cacheTtl(env)}`,
      Vary: 'Accept',
      'Access-Control-Allow-Origin': '*',
      'X-Content-Type-Options': 'nosniff',
    },
  });

  if (request.method === 'GET') ctx.waitUntil(cache.put(cacheKey, response.clone()));
  return response;
}

type WaitUntilContext = {
  waitUntil(promise: Promise<unknown>): void;
};

function defaultCache(): Cache {
  return (caches as CacheStorage & { default: Cache }).default;
}

function addCors(response: Response): Response {
  const headers = new Headers(response.headers);
  headers.set('Access-Control-Allow-Origin', '*');
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}
