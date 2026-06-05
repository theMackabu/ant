import { z } from 'zod';
import { Hono } from 'hono';
import type { Env } from './types';
import type { Context } from 'hono';
import { HttpError } from './errors';
import { cachedJson } from './http-cache';
import { routeIndex } from './route-index';
import { branch, repository, withActionsRun, withArtifactVersion, withBranch } from './config';
import { downloadArtifact } from './downloads';
import { resolveArch, resolveTarget } from './targets';
import {
  BranchQuerySchema,
  DownloadParamsSchema,
  RefreshQuerySchema,
  VersionQuerySchema,
} from './schemas';
import { cachedManifest, forceRefreshManifest } from './manifest-cache';
import {
  latestAntVersion,
  latestManifest,
  resolveAnt,
  resolveNanosArtifact,
  versionCheck,
  versionManifest,
} from './resolver';

const app = new Hono<{ Bindings: Env }>();
type AppContext = Context<{ Bindings: Env }>;

app.options('*', c => {
  c.header('Access-Control-Allow-Origin', '*');
  c.header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  c.header('Access-Control-Allow-Headers', 'Authorization, Content-Type');
  c.header('Access-Control-Max-Age', '86400');
  return c.body(null, 204);
});

app.use('*', async (c, next) => {
  await next();
  c.header('Access-Control-Allow-Origin', '*');
  c.header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  c.header('Access-Control-Allow-Headers', 'Authorization, Content-Type');
  c.header('X-Content-Type-Options', 'nosniff');
});

app.onError((error, c) => {
  if (error instanceof HttpError) return c.json({ error: error.message }, error.status as 400);
  if (error instanceof z.ZodError)
    return c.json({ error: 'invalid request', issues: error.issues }, 400);

  console.error(error);
  return c.json({ error: 'internal error' }, 500);
});

app.notFound(c => c.json({ error: 'not found' }, 404));

app.get('/', c => c.json(routeIndex(new URL(c.req.url))));

app.get('/v1/latest', c => {
  const env = requestEnv(c);
  return cachedManifest(env, c.executionCtx, () => latestManifest(new URL(c.req.url), env));
});

app.post('/v1/refresh', c => {
  const env = refreshEnv(c);
  const auth = c.req.header('Authorization') || '';
  const expected = env.MANIFEST_REFRESH_TOKEN;
  if (!expected || auth !== `Bearer ${expected}`) return c.json({ error: 'unauthorized' }, 401);

  return forceRefreshManifest(env, c.executionCtx, () => latestManifest(new URL(c.req.url), env));
});

app.get('/v1/version', c => versionRoute(c));
app.get('/v1/check', c => versionRoute(c));

app.get('/v1/version/get-tag', c => {
  const cacheKey = `version-latest:${repository(c.env)}:release`;

  return cachedJson(c.req.raw, c.executionCtx, c.env, cacheKey, () => latestAntVersion(c.env));
});

app.get('/v1/version/:version', c => {
  const version = c.req.param('version');
  if (version === 'latest') {
    throw new HttpError('latest is not a version route; use /v1/version/get-tag', 404);
  }

  const cacheKey = `version-manifest:${repository(c.env)}:${version}`;

  return cachedJson(c.req.raw, c.executionCtx, c.env, cacheKey, () =>
    versionManifest(c.env, version),
  );
});

app.on(['GET', 'HEAD'], '/v1/download/:kind/:name', async c => {
  const env = requestEnv(c);
  const params = DownloadParamsSchema.parse(c.req.param());
  const url = new URL(c.req.url);
  const artifact =
    params.kind === 'ant'
      ? await resolveAnt(env, resolveTarget(params.name), url)
      : params.kind === 'sandbox'
        ? await resolveNanosArtifact(env, 'sandbox', resolveArch(params.name), url)
        : await resolveNanosArtifact(env, 'kernel', resolveArch(params.name), url);

  return downloadArtifact(c.req.raw, env, c.executionCtx, artifact);
});

async function versionRoute(c: AppContext) {
  const query = VersionQuerySchema.parse(c.req.query());
  const env = requestEnv(c);
  const cacheKey = `version:${repository(env)}:${branch(env)}:${query.kind || ''}:${query.arch || ''}:${query.target}:${query.current}`;

  return cachedJson(c.req.raw, c.executionCtx, c.env, cacheKey, () =>
    versionCheck(new URL(c.req.url), env, query),
  );
}

function requestEnv(c: AppContext): Env {
  const query = BranchQuerySchema.parse(c.req.query());
  const headerBranch = c.req.header('X-Ant-Branch');
  const headerRunId = c.req.header('X-GitHub-Run-Id');
  const requestedBranch =
    query.branch ||
    (headerBranch ? BranchQuerySchema.parse({ branch: headerBranch }).branch : undefined);
  const requestedRunId =
    query.run_id ||
    (headerRunId ? BranchQuerySchema.parse({ run_id: headerRunId }).run_id : undefined);
  const branchEnv = requestedBranch ? withBranch(c.env, requestedBranch) : c.env;
  return requestedRunId ? withActionsRun(branchEnv, requestedRunId) : branchEnv;
}

function refreshEnv(c: AppContext): Env {
  const env = requestEnv(c);
  const query = RefreshQuerySchema.parse(c.req.query());
  const headerVersion = c.req.header('X-Ant-Version');
  const requestedVersion =
    query.version ||
    (headerVersion ? RefreshQuerySchema.parse({ version: headerVersion }).version : undefined);
  return requestedVersion ? withArtifactVersion(env, requestedVersion) : env;
}

export default app;
