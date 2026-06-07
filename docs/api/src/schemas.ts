import { z } from 'zod';

const BranchSchema = z
  .string()
  .trim()
  .min(1)
  .max(255)
  .regex(/^[A-Za-z0-9._/-]+$/, 'branch contains unsupported characters')
  .refine(value => !value.includes('..'), 'branch cannot contain ..');

const ArtifactRevisionSchema = z
  .string()
  .trim()
  .regex(/^[0-9a-fA-F]{40,64}$/, 'revision must be a full git hash');

export const VersionQuerySchema = z
  .object({
    current: z.string().optional(),
    version: z.string().optional(),
    target: z.string().optional(),
  })
  .superRefine((query, ctx) => {
    if (!query.current && !query.version) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['current'],
        message: 'current version is required',
      });
    }

    if (!query.target) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['target'],
        message: 'target is required for ant version checks',
      });
    }
  })
  .transform(query => ({
    current: query.current || query.version || '',
    target: query.target || '',
  }));

export const DownloadParamsSchema = z.object({
  kind: z.enum(['ant', 'sandbox', 'kernel']),
  name: z.string().min(1),
});

export const BranchQuerySchema = z.object({
  branch: BranchSchema.optional(),
  run_id: z
    .string()
    .regex(/^[1-9][0-9]*$/)
    .optional(),
});

export const RefreshQuerySchema = BranchQuerySchema.extend({
  revision: ArtifactRevisionSchema.optional(),
});
