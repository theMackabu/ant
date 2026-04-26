import { Hono } from 'hono';
import { renderOgPng } from './og';
import type { Bindings } from './env';
import { drizzle } from 'drizzle-orm/d1';
import { CrashReportSchema } from './schema';
import { renderBlank, renderPrivacy, renderReport } from './view';
import { createReport, getRawReport, getReport } from './reports';

import {
  getAntLogoDataUrl,
  getOgFontBytes,
  ogImageId,
  publicOgImageUrl,
  publicUrl,
} from './helpers';

const app = new Hono<{ Bindings: Bindings }>();

app.get('/', c => c.html(renderBlank(), 404));
app.get('/privacy', c => c.html(renderPrivacy()));

app.get('/og/:image', async c => {
  const db = drizzle(c.env.DB);
  const id = ogImageId(c.req.param('image'));

  const cache = caches.default;
  const cacheKey = new Request(publicOgImageUrl(c.req.url, id), { method: 'GET' });

  const cached = await cache.match(cacheKey);
  if (cached) return cached;

  const report = await getReport(db, id);
  if (!report) return c.notFound();

  const png = await renderOgPng(
    report,
    await getAntLogoDataUrl(c.env, c.req.url),
    await getOgFontBytes(c.env, c.req.url),
  );

  const response = c.body(png, 200, {
    'Content-Type': 'image/png',
    'Cache-Control': 'public, max-age=2592000, immutable',
  });

  c.executionCtx.waitUntil(cache.put(cacheKey, response.clone()));
  return response;
});

app.get('/:id', async c => {
  const db = drizzle(c.env.DB);
  const pathId = c.req.param('id');

  if (pathId.endsWith('.json')) {
    const report = await getRawReport(db, pathId.slice(0, -5));
    if (!report) return c.json({ error: 'not found' }, 404);
    return c.json(report, 200);
  }

  const id = pathId;
  const report = await getReport(db, id);

  if (!report) return c.html(renderBlank(), 404);
  return c.html(renderReport(report, publicUrl(c.req.url, id), publicOgImageUrl(c.req.url, id)));
});

app.post('/report', async c => {
  let rawReport: unknown;
  try {
    rawReport = await c.req.json();
  } catch {
    return c.json({ error: 'invalid payload' }, 400);
  }

  const parsedReport = CrashReportSchema.safeParse(rawReport);
  if (!parsedReport.success) return c.json({ error: 'invalid report' }, 400);

  const db = drizzle(c.env.DB);
  const id = await createReport(db, parsedReport.data);
  return c.text(publicUrl(c.req.url, id), 200);
});

export default app;
