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

const ArtifactVersionSchema = z
  .string()
  .trim()
  .min(1)
  .max(128)
  .regex(/^[A-Za-z0-9._+-]+$/, 'version contains unsupported characters');

export const VersionQuerySchema = z
  .object({
    current: z.string().optional(),
    version: z.string().optional(),
    build_timestamp: z
      .string()
      .regex(/^[0-9]+$/)
      .optional(),
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
    buildTimestamp: query.build_timestamp ? Number(query.build_timestamp) : undefined,
    target: query.target || '',
  }));

export const DownloadParamsSchema = z.object({
  kind: z.enum(['ant', 'runtime', 'sandbox', 'kernel']),
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
  version: ArtifactVersionSchema.optional(),
  revision: ArtifactRevisionSchema.optional(),
});

export const ReleaseNotesQuerySchema = z.object({
  version: ArtifactVersionSchema.optional(),
  revision: ArtifactRevisionSchema.optional(),
});
