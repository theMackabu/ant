import { z } from 'zod';

const BranchSchema = z
  .string()
  .trim()
  .min(1)
  .max(255)
  .regex(/^[A-Za-z0-9._/-]+$/, 'branch contains unsupported characters')
  .refine(value => !value.includes('..'), 'branch cannot contain ..');

export const VersionQuerySchema = z
  .object({
    current: z.string().optional(),
    version: z.string().optional(),
    target: z.string().optional(),
    kind: z.enum(['ant', 'sandbox', 'kernel']),
    arch: z.string().optional(),
  })
  .superRefine((query, ctx) => {
    if (!query.current && !query.version) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['current'],
        message: 'current version is required',
      });
    }

    if (query.kind === 'ant' && !query.target) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['target'],
        message: 'target is required for ant version checks',
      });
    }

    if (query.kind !== 'ant' && !query.arch && !query.target) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['arch'],
        message: 'arch is required for sandbox and kernel version checks',
      });
    }
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

export const BranchQuerySchema = z.object({
  branch: BranchSchema.optional(),
});
