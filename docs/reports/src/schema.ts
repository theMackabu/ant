import { z } from 'zod';
import { integer, primaryKey, sqliteTable, text, uniqueIndex } from 'drizzle-orm/sqlite-core';

const boundedString = z
  .string()
  .min(1)
  .transform(value => value.slice(0, 512));

const boundedFrameString = z.string().transform(value => value.slice(0, 512));
const reportNumber = z.number().int().nonnegative().nullable();

export const CrashReportSchema = z
  .object({
    schema: z.literal(1),
    runtime: z.literal('ant'),
    version: boundedString,
    target: boundedString,
    os: boundedString,
    arch: boundedString,
    kind: boundedString,
    code: boundedString,
    reason: boundedString,
    addr: boundedString,
    elapsedMs: reportNumber,
    peakRss: reportNumber,
    frames: z.array(boundedFrameString).transform(value => value.slice(0, 48)),
  })
  .strict();

export const reports = sqliteTable(
  'reports',
  {
    id: text('id').primaryKey(),
    runtime: text('runtime').notNull(),
    version: text('version').notNull(),
    trace: text('trace').notNull(),
    kind: text('kind').notNull(),
    reason: text('reason').notNull(),
    code: text('code').notNull(),
    target: text('target').notNull(),
    os: text('os').notNull(),
    arch: text('arch').notNull(),
    faultAddress: text('fault_address').notNull(),
    elapsedMs: integer('elapsed_ms'),
    peakRss: integer('peak_rss'),
    firstSeenAt: text('first_seen_at').notNull(),
    lastSeenAt: text('last_seen_at').notNull(),
    hitCount: integer('hit_count').notNull(),
    expiresAt: text('expires_at').notNull(),
  },
  table => [uniqueIndex('reports_trace_unique').on(table.trace)],
);

export const frames = sqliteTable('frames', {
  hash: text('hash').primaryKey(),
  frame: text('frame').notNull(),
});

export const reportFrames = sqliteTable(
  'report_frames',
  {
    reportId: text('report_id')
      .notNull()
      .references(() => reports.id, { onDelete: 'cascade' }),
    frameIndex: integer('frame_index').notNull(),
    frameHash: text('frame_hash')
      .notNull()
      .references(() => frames.hash, { onDelete: 'restrict' }),
  },
  table => [primaryKey({ columns: [table.reportId, table.frameIndex] })],
);

export type CrashReport = z.infer<typeof CrashReportSchema>;
