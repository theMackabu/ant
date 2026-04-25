import { Hono } from 'hono';
import { asc, eq } from 'drizzle-orm';
import { drizzle } from 'drizzle-orm/d1';
import { renderBlank, renderReport } from './view';
import { generate as generateShortUuid } from 'short-uuid';
import { CrashReportSchema, reportFrames, reports, type CrashReport } from './schema';

type Bindings = { DB: D1Database };
const app = new Hono<{ Bindings: Bindings }>();

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

  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(traceInput));
  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

function publicUrl(requestUrl: string, id: string): string {
  const url = new URL(requestUrl);
  return `${url.origin}/${id}`;
}

const reportFromRows = (
  row: typeof reports.$inferSelect,
  frames: (typeof reportFrames.$inferSelect)[],
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
  frames: frames.map(frame => frame.frame),
});

async function insertReportFrames(
  db: ReturnType<typeof drizzle>,
  reportId: string,
  frames: string[],
): Promise<void> {
  if (!frames.length) return;
  await db
    .insert(reportFrames)
    .values(frames.map((frame, index) => ({ frame, reportId, frameIndex: index })));
}

async function getReportFrames(
  db: ReturnType<typeof drizzle>,
  reportId: string,
): Promise<(typeof reportFrames.$inferSelect)[]> {
  return db
    .select()
    .from(reportFrames)
    .where(eq(reportFrames.reportId, reportId))
    .orderBy(asc(reportFrames.frameIndex));
}

app.get('/:id', async c => {
  const db = drizzle(c.env.DB);

  const [row] = await db
    .select()
    .from(reports)
    .where(eq(reports.id, c.req.param('id')))
    .limit(1);

  if (!row) return c.html(renderBlank(), 404);
  const frames = await getReportFrames(db, row.id);

  return c.html(renderReport(reportFromRows(row, frames), publicUrl(c.req.url, row.id)));
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
