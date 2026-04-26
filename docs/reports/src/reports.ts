import { sha256 } from './helpers';
import { asc, eq } from 'drizzle-orm';
import { drizzle } from 'drizzle-orm/d1';
import { generate as generateShortUuid } from 'short-uuid';
import { frames as frameTable, reportFrames, reports, type CrashReport } from './schema';

type ReportDb = ReturnType<typeof drizzle>;

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

async function insertReportFrames(db: ReportDb, reportId: string, frames: string[]): Promise<void> {
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

async function getReportFrames(db: ReportDb, reportId: string): Promise<{ frame: string }[]> {
  return db
    .select({
      frame: frameTable.frame,
    })
    .from(reportFrames)
    .innerJoin(frameTable, eq(reportFrames.frameHash, frameTable.hash))
    .where(eq(reportFrames.reportId, reportId))
    .orderBy(asc(reportFrames.frameIndex));
}

export async function getReport(db: ReportDb, id: string): Promise<CrashReport | null> {
  const [row] = await db.select().from(reports).where(eq(reports.id, id)).limit(1);

  if (!row) return null;
  const frames = await getReportFrames(db, row.id);
  return reportFromRows(row, frames);
}

export async function createReport(db: ReportDb, report: CrashReport): Promise<string> {
  const id = generateShortUuid();
  const trace = await reportTrace(report);
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
  return id;
}
