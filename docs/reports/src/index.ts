import { Hono } from 'hono';
import { renderOgPng } from './og';
import { asc, eq } from 'drizzle-orm';
import { drizzle } from 'drizzle-orm/d1';
import { renderBlank, renderReport } from './view';
import { generate as generateShortUuid } from 'short-uuid';

import {
  CrashReportSchema,
  frames as frameTable,
  reportFrames,
  reports,
  type CrashReport,
} from './schema';

type Bindings = { ASSETS: Fetcher; DB: D1Database };
const app = new Hono<{ Bindings: Bindings }>();

let antLogoDataUrl: Promise<string> | null = null;
let ogFontBytes: Promise<Uint8Array[]> | null = null;

app.get('/', c => c.html(renderBlank(), 404));

async function reportTrace(report: CrashReport): Promise<string> {
  const traceInput = JSON.stringify({
    runtime: report.runtime,
    version: report.version,
    target: report.target,
    os: report.os,
    arch: report.arch,
    code: report.code,
    reason: report.reason,
    addr: report.addr,
    frames: report.frames,
  });

  return sha256(traceInput);
}

async function sha256(input: string): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(input));
  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

function publicUrl(requestUrl: string, id: string): string {
  const url = new URL(requestUrl);
  return `${url.origin}/${id}`;
}

function publicOgImageUrl(requestUrl: string, id: string): string {
  const url = new URL(requestUrl);
  return `${url.origin}/og/${id}.png`;
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

async function loadAntLogoDataUrl(env: Bindings, requestUrl: string): Promise<string> {
  const response = await env.ASSETS.fetch(new Request(new URL('/assets/ant.png', requestUrl)));
  if (!response.ok) return '';
  const bytes = new Uint8Array(await response.arrayBuffer());
  return `data:image/png;base64,${bytesToBase64(bytes)}`;
}

async function loadAssetBytes(
  env: Bindings,
  requestUrl: string,
  path: string,
): Promise<Uint8Array> {
  const response = await env.ASSETS.fetch(new Request(new URL(path, requestUrl)));
  if (!response.ok) return new Uint8Array();
  return new Uint8Array(await response.arrayBuffer());
}

function getAntLogoDataUrl(env: Bindings, requestUrl: string): Promise<string> {
  antLogoDataUrl ??= loadAntLogoDataUrl(env, requestUrl);
  return antLogoDataUrl;
}

function ogImageId(value: string): string {
  return value.endsWith('.png') ? value.slice(0, -4) : value;
}

function getOgFontBytes(env: Bindings, requestUrl: string): Promise<Uint8Array[]> {
  ogFontBytes ??= Promise.all([
    loadAssetBytes(env, requestUrl, '/assets/Arial.ttf'),
    loadAssetBytes(env, requestUrl, '/assets/Arial-Bold.ttf'),
    loadAssetBytes(env, requestUrl, '/assets/BerkeleyMono-Regular.woff2'),
  ]);
  return ogFontBytes;
}

const reportFromRows = (
  row: typeof reports.$inferSelect,
  frameRows: { frame: string }[],
): CrashReport => ({
  schema: 1,
  runtime: 'ant',
  version: row.version,
  target: row.target,
  os: row.os,
  arch: row.arch,
  kind: row.kind,
  code: row.code,
  reason: row.reason,
  addr: row.faultAddress,
  elapsedMs: row.elapsedMs,
  peakRss: row.peakRss,
  frames: frameRows.map(row => row.frame),
});

async function insertReportFrames(
  db: ReturnType<typeof drizzle>,
  reportId: string,
  frames: string[],
): Promise<void> {
  if (!frames.length) return;
  const frameHashes = await Promise.all(frames.map(frame => sha256(frame)));
  const uniqueFrames = new Map<string, string>();

  frames.forEach((frame, index) => {
    uniqueFrames.set(frameHashes[index], frame);
  });

  await db
    .insert(frameTable)
    .values([...uniqueFrames].map(([hash, frame]) => ({ hash, frame })))
    .onConflictDoNothing();

  await db.insert(reportFrames).values(
    frameHashes.map((frameHash, index) => ({
      frameHash,
      reportId,
      frameIndex: index,
    })),
  );
}

async function getReportFrames(
  db: ReturnType<typeof drizzle>,
  reportId: string,
): Promise<{ frame: string }[]> {
  return db
    .select({
      frame: frameTable.frame,
    })
    .from(reportFrames)
    .innerJoin(frameTable, eq(reportFrames.frameHash, frameTable.hash))
    .where(eq(reportFrames.reportId, reportId))
    .orderBy(asc(reportFrames.frameIndex));
}

async function getReport(db: ReturnType<typeof drizzle>, id: string): Promise<CrashReport | null> {
  const [row] = await db.select().from(reports).where(eq(reports.id, id)).limit(1);

  if (!row) return null;
  const frames = await getReportFrames(db, row.id);
  return reportFromRows(row, frames);
}

app.get('/og/:image', async c => {
  const db = drizzle(c.env.DB);
  const id = ogImageId(c.req.param('image'));
  const report = await getReport(db, id);
  if (!report) return c.notFound();

  const png = await renderOgPng(
    report,
    await getAntLogoDataUrl(c.env, c.req.url),
    await getOgFontBytes(c.env, c.req.url),
  );

  return c.body(png, 200, {
    'Content-Type': 'image/png',
    'Cache-Control': 'public, max-age=3600',
  });
});

app.get('/:id', async c => {
  const db = drizzle(c.env.DB);
  const id = c.req.param('id');
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

  const report = parsedReport.data;
  const trace = await reportTrace(report);

  const db = drizzle(c.env.DB);
  const id = generateShortUuid();

  const now = new Date();
  const expires = new Date(now.getTime() + 30 * 24 * 60 * 60 * 1000);

  await db.insert(reports).values({
    id,
    trace,
    hitCount: 1,
    runtime: report.runtime,
    version: report.version,
    kind: report.kind,
    reason: report.reason,
    code: report.code,
    target: report.target,
    os: report.os,
    arch: report.arch,
    faultAddress: report.addr,
    elapsedMs: report.elapsedMs,
    peakRss: report.peakRss,
    firstSeenAt: now.toISOString(),
    lastSeenAt: now.toISOString(),
    expiresAt: expires.toISOString(),
  });

  await insertReportFrames(db, id, report.frames);
  return c.text(publicUrl(c.req.url, id), 200);
});

export default app;
