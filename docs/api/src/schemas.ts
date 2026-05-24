import { z } from 'zod';

export const VersionQuerySchema = z
  .object({
    current: z.string().optional(),
    version: z.string().optional(),
    target: z.string().optional(),
    kind: z.enum(['ant', 'sandbox', 'kernel']).default('ant'),
    arch: z.string().optional(),
  })
  .transform(query => ({
    current: query.current || query.version || '',
    target: query.target || '',
    kind: query.kind,
    arch: query.arch,
  }));

export const DownloadParamsSchema = z.object({
  kind: z.enum(['ant', 'sandbox', 'kernel']),
  name: z.string().min(1),
});
