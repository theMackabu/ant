import {
  actionsRunId,
  branch,
  BUILD_WORKFLOW,
  canDownloadActionsArtifacts,
  DEFAULT_BRANCH,
  DEFAULT_CHANNEL,
  manifestKey,
  MUSL_SANDBOX_WORKFLOW,
  GITHUB_REPOSITORY,
} from './config';
import type { Channel, RequestOptions } from './config';
import { HttpError, isNotFound } from './errors';
import {
  fetchArtifactDownload,
  fetchReleaseAssetDownload,
  findLatestAnyRunWithArtifacts,
  findLatestRunWithArtifacts,
  findReleaseAsset,
  findRunWithArtifacts,
  latestRelease,
  releaseByTag,
  releaseTagRevision,
  readVersionArtifact,
  requireArtifact,
} from './github';
import { antTargets, resolveTarget } from './targets';
import { isOutOfDate, normalizeVersion } from './version';
import type {
  ActionSourceInfo,
  AntTarget,
  Artifact,
  ArtifactKind,
  Env,
  GitHubRelease,
  ReleaseSourceInfo,
  ResolvedArtifact,
  WorkflowRun,
} from './types';

export async function resolveFromManifest(
  env: Env,
  kind: ArtifactKind,
  name: string,
  channel: Channel,
  options: RequestOptions = {},
): Promise<ResolvedArtifact | null> {
  const object = await env.DOWNLOADS.get(manifestKey(channel));
  if (!object) return null;

  let manifest: unknown;
  try {
    manifest = JSON.parse(await object.text());
  } catch {
    return null;
  }

  if (!manifest || typeof manifest !== 'object') return null;
  const section = (manifest as Record<string, unknown>)[kind];
  if (!Array.isArray(section)) return null;

  const field = kind === 'ant' || kind === 'runtime' ? 'target' : 'arch';
  const entry = section.find(
    item =>
      Boolean(item) &&
      typeof item === 'object' &&
      (item as Record<string, unknown>).available === true &&
      (item as Record<string, unknown>)[field] === name,
  ) as ResolvedArtifact | undefined;

  if (!entry || !entry.artifact || !entry.source) return null;

  // An explicit pin must match, otherwise fall through to a live lookup.
  if (entry.source.type === 'actions') {
    if (options.runId && entry.source.run_id !== options.runId) return null;
    if (options.branch && entry.source.head_branch !== options.branch) return null;
  } else if (options.runId) return null;

  return entry;
}

export async function latestManifest(url: URL, env: Env, options: RequestOptions = {}) {
  const ant = await Promise.all(
    antTargets.map(target =>
      resolveManifestEntry(
        {
          target: target.key,
          os: target.os,
          arch: target.arch,
          libc: target.libc,
        },
        () => resolveAnt(env, target, url, options),
      ),
    ),
  );

  const runtime = await Promise.all(
    antTargets.map(target =>
      resolveManifestEntry(
        {
          target: target.key,
          os: target.os,
          arch: target.arch,
          libc: target.libc,
        },
        () => resolveRuntime(env, target, url, options),
      ),
    ),
  );

  const sandbox = await Promise.all(
    ['x64', 'aarch64'].map(arch =>
      resolveManifestEntry({ arch }, () =>
        resolveNanosArtifact(env, 'sandbox', arch, url, options),
      ),
    ),
  );

  const kernel = await Promise.all(
    ['x64', 'aarch64'].map(arch =>
      resolveManifestEntry({ arch }, () => resolveNanosArtifact(env, 'kernel', arch, url, options)),
    ),
  );

  return {
    schema: 1,
    channel: options.channel || DEFAULT_CHANNEL,
    generated_at: new Date().toISOString(),
    ant,
    runtime,
    sandbox,
    kernel,
  };
}

export async function versionManifest(env: Env, version: string) {
  const release = await releaseByTag(env, version);
  const ant = antTargets.map(target =>
    resolveManifestEntry(
      {
        target: target.key,
        os: target.os,
        arch: target.arch,
        libc: target.libc,
      },
      () => resolveReleaseAsset(env, release, 'ant', target.artifact),
    ),
  );

  const sandbox = arches.map(arch =>
    resolveManifestEntry({ arch }, () =>
      resolveReleaseAsset(env, release, 'sandbox', `ant-sandbox-${arch}`),
    ),
  );

  const kernel = arches.map(arch =>
    resolveManifestEntry({ arch }, () =>
      resolveReleaseAsset(env, release, 'kernel', `ant-kernel-${arch}`),
    ),
  );

  return {
    schema: 1,
    version: releaseInfo(release),
    generated_at: new Date().toISOString(),
    ant: await Promise.all(ant),
    sandbox: await Promise.all(sandbox),
    kernel: await Promise.all(kernel),
  };
}

async function resolveManifestEntry<T extends Record<string, unknown>>(
  base: T,
  resolve: () => ResolvedArtifact | Promise<ResolvedArtifact>,
): Promise<T & (({ available: true } & ResolvedArtifact) | { available: false; error: string })> {
  try {
    return {
      ...base,
      available: true,
      ...(await resolve()),
    };
  } catch (error) {
    if (!isNotFound(error)) throw error;
    return {
      ...base,
      available: false,
      error: error instanceof Error ? error.message : 'not found',
    };
  }
}

type VersionCheckQuery = {
  target?: string;
  current: string;
  buildTimestamp?: number;
};

type ReleaseArtifactKind = Exclude<ArtifactKind, 'runtime'>;

const arches = ['x64', 'aarch64'];

export async function latestAntVersion(env: Env) {
  const release = await latestRelease(env);

  return {
    schema: 1,
    ...releaseInfo(release),
  };
}

export async function versionCheck(
  url: URL,
  env: Env,
  query: VersionCheckQuery,
  options: RequestOptions = {},
  channel: Channel = DEFAULT_CHANNEL,
) {
  const target = resolveTarget(query.target || '');
  const [release, latest] = await Promise.all([
    latestRelease(env),
    resolveFromManifest(env, 'ant', target.key, channel, options).then(
      entry => entry ?? resolveAnt(env, target, url, options),
    ),
  ]);
  const current = normalizeVersion(query.current);
  const latestVersion = normalizeVersion(release.tag_name);

  return {
    schema: 1,
    kind: 'ant',
    target: query.target,
    current: current || null,
    latest: latestVersion,
    latest_sha:
      latest.revision || (latest.source.type === 'actions' ? latest.source.head_sha : null),
    latest_build_timestamp: latest.build_timestamp ?? null,
    out_of_date: isOutOfDate(
      current,
      latestVersion,
      undefined,
      query.buildTimestamp,
      latest.build_timestamp,
    ),
    download_url: latest.download_url,
    source: latest.source,
  };
}

export async function resolveAnt(
  env: Env,
  target: AntTarget,
  url: URL,
  options: RequestOptions = {},
): Promise<ResolvedArtifact> {
  if (!canDownloadActionsArtifacts(env)) {
    return resolveReleaseArtifact(env, 'ant', target.artifact, target.key, url, options);
  }

  try {
    return await resolveActionBinary(env, target, url, options, 'ant');
  } catch (error) {
    if (!isNotFound(error)) throw error;
    return resolveReleaseArtifact(env, 'ant', target.artifact, target.key, url, options);
  }
}

export async function resolveRuntime(
  env: Env,
  target: AntTarget,
  url: URL,
  options: RequestOptions = {},
): Promise<ResolvedArtifact> {
  if (!canDownloadActionsArtifacts(env)) {
    throw new HttpError('runtime artifacts require GITHUB_TOKEN for Actions downloads', 404);
  }
  return resolveActionBinary(env, target, url, options, 'runtime');
}

export async function resolveNamedArtifact(
  env: Env,
  kind: ReleaseArtifactKind,
  workflow: string,
  artifactName: string,
  url: URL,
  options: RequestOptions = {},
): Promise<ResolvedArtifact> {
  const arch = artifactName.endsWith('aarch64') ? 'aarch64' : 'x64';

  if (!canDownloadActionsArtifacts(env)) {
    return resolveReleaseAfterActionsMiss(
      env,
      kind,
      artifactName,
      arch,
      url,
      options,
      'actions artifact lookup skipped because GITHUB_TOKEN is not configured',
    );
  }

  try {
    return await resolveActionNamedArtifact(env, kind, workflow, artifactName, arch, url, options);
  } catch (error) {
    if (!isNotFound(error)) throw error;
    const primaryReason = error instanceof Error ? error.message : 'actions artifact not found';

    try {
      return await resolveAnyActionNamedArtifact(env, kind, artifactName, arch, url, options);
    } catch (fallbackError) {
      if (!isNotFound(fallbackError)) throw fallbackError;
      const fallbackReason =
        fallbackError instanceof Error ? fallbackError.message : 'actions artifact not found';
      return resolveReleaseAfterActionsMiss(
        env,
        kind,
        artifactName,
        arch,
        url,
        options,
        `${primaryReason}; ${fallbackReason}`,
      );
    }
  }
}

export async function resolveNanosArtifact(
  env: Env,
  kind: Extract<ArtifactKind, 'sandbox' | 'kernel'>,
  arch: string,
  url: URL,
  options: RequestOptions = {},
): Promise<ResolvedArtifact> {
  const artifact = await resolveNamedArtifact(
    env,
    kind,
    MUSL_SANDBOX_WORKFLOW,
    `ant-sandbox-${arch}`,
    url,
    options,
  );

  const filename = kind === 'kernel' ? `ant-kernel-${arch}.img` : `ant-sandbox-${arch}.img`;
  return {
    ...artifact,
    name: `${kind === 'kernel' ? 'ant-kernel' : 'ant-sandbox'}-${arch}`,
    gzip_url: gzipUrl(artifact.download_url),
    zip_entry: filename,
    filename,
  };
}

export async function fetchDownload(env: Env, artifact: ResolvedArtifact): Promise<Response> {
  if (artifact.source.type === 'actions') {
    return fetchArtifactDownload(env, artifact.artifact.id);
  }

  const releaseUrl =
    env.GITHUB_TOKEN && artifact.artifact.api_url
      ? artifact.artifact.api_url
      : artifact.artifact.browser_download_url || artifact.artifact.api_url;
  if (!releaseUrl) {
    throw new HttpError(`release asset missing download URL: ${artifact.name}`, 502);
  }
  return fetchReleaseAssetDownload(env, releaseUrl);
}

export function downloadUrl(
  url: URL,
  kind: ArtifactKind,
  name: string,
  artifactBranch: string,
  runId?: number,
): string {
  const next = new URL(`/v1/download/${kind}/${encodeURIComponent(name)}`, url);
  if (url.searchParams.has('branch') || artifactBranch !== DEFAULT_BRANCH) {
    next.searchParams.set('branch', artifactBranch);
  }
  if (runId) {
    next.searchParams.set('run_id', String(runId));
  }
  return next.toString();
}

function gzipUrl(downloadUrl: string): string {
  const url = new URL(downloadUrl);
  url.searchParams.set('gzip', '1');
  return url.toString();
}

async function resolveActionBinary(
  env: Env,
  target: AntTarget,
  url: URL,
  options: RequestOptions,
  kind: 'ant' | 'runtime',
): Promise<ResolvedArtifact> {
  const runId = actionsRunId(env, options);
  if (runId) {
    try {
      return await resolveActionBinaryFromRun(env, target, url, runId, options, kind);
    } catch (error) {
      if (!isNotFound(error)) throw error;
    }
  }

  const match = await findLatestRunWithArtifacts(env, BUILD_WORKFLOW, branch(env, options), [
    kind === 'runtime' ? `ant-runtime-${target.key}` : target.artifact,
  ]);
  return resolveActionBinaryFromArtifacts(
    env,
    target,
    url,
    match.run,
    match.artifacts,
    options,
    kind,
  );
}

async function resolveActionBinaryFromRun(
  env: Env,
  target: AntTarget,
  url: URL,
  runId: number,
  options: RequestOptions,
  kind: 'ant' | 'runtime',
): Promise<ResolvedArtifact> {
  const artifactName = kind === 'runtime' ? `ant-runtime-${target.key}` : target.artifact;
  const match = await findRunWithArtifacts(env, runId, [artifactName]);
  return resolveActionBinaryFromArtifacts(
    env,
    target,
    url,
    match.run,
    match.artifacts,
    options,
    kind,
  );
}

async function resolveActionBinaryFromArtifacts(
  env: Env,
  target: AntTarget,
  url: URL,
  run: WorkflowRun,
  artifacts: Artifact[],
  options: RequestOptions,
  kind: 'ant' | 'runtime',
): Promise<ResolvedArtifact> {
  const artifactName = kind === 'runtime' ? `ant-runtime-${target.key}` : target.artifact;
  const artifact = requireArtifact(artifacts, artifactName);
  const versionArtifact = artifacts.find(
    item => item.name === `version-${target.artifact}` && !item.expired,
  );
  let version = options.version;
  let buildTimestamp: number | undefined;

  if (versionArtifact) {
    try {
      const info = await readVersionArtifact(env, versionArtifact);
      version = info.version;
      buildTimestamp = info.buildTimestamp;
    } catch (error) {
      console.warn(`failed to read ${versionArtifact.name}`, error);
    }
  }

  const resolved = resolvedAction(
    kind,
    artifact,
    version,
    buildTimestamp,
    options.revision || run.head_sha,
    actionSourceInfo(BUILD_WORKFLOW, run),
    downloadUrl(url, kind, target.key, branch(env, options), run.id),
  );

  const binary = kind === 'runtime' ? 'ant-runtime' : 'ant';
  resolved.zip_entry = target.os === 'windows' ? `${binary}.exe` : binary;
  resolved.filename = resolved.zip_entry;

  return resolved;
}

async function resolveActionNamedArtifact(
  env: Env,
  kind: ArtifactKind,
  workflow: string,
  artifactName: string,
  arch: string,
  url: URL,
  options: RequestOptions,
): Promise<ResolvedArtifact> {
  const runId = actionsRunId(env, options);
  if (runId) {
    try {
      const match = await findRunWithArtifacts(env, runId, [artifactName]);
      const artifact = requireArtifact(match.artifacts, artifactName);
      return resolvedAction(
        kind,
        artifact,
        undefined,
        undefined,
        options.revision || match.run.head_sha,
        actionSourceInfo(match.run.path || workflow, match.run),
        downloadUrl(url, kind, arch, branch(env, options), runId),
      );
    } catch (error) {
      if (!isNotFound(error)) throw error;
    }
  }

  const match = await findLatestRunWithArtifacts(env, workflow, branch(env, options), [
    artifactName,
  ]);
  const artifact = requireArtifact(match.artifacts, artifactName);
  return resolvedAction(
    kind,
    artifact,
    undefined,
    undefined,
    options.revision || match.run.head_sha,
    actionSourceInfo(workflow, match.run),
    downloadUrl(url, kind, arch, branch(env, options), match.run.id),
  );
}

async function resolveAnyActionNamedArtifact(
  env: Env,
  kind: ArtifactKind,
  artifactName: string,
  arch: string,
  url: URL,
  options: RequestOptions,
): Promise<ResolvedArtifact> {
  const match = await findLatestAnyRunWithArtifacts(env, [artifactName]);
  const artifact = requireArtifact(match.artifacts, artifactName);
  return resolvedAction(
    kind,
    artifact,
    undefined,
    undefined,
    options.revision || match.run.head_sha,
    actionSourceInfo(match.run.path || match.run.name, match.run),
    downloadUrl(url, kind, arch, branch(env, options), match.run.id),
  );
}

function resolvedAction(
  kind: ArtifactKind,
  artifact: Artifact,
  version: string | undefined,
  buildTimestamp: number | undefined,
  revision: string,
  source: ActionSourceInfo,
  download_url: string,
): ResolvedArtifact {
  return {
    kind,
    name: artifact.name,
    version: kind === 'ant' || kind === 'runtime' ? version : undefined,
    build_timestamp: kind === 'ant' || kind === 'runtime' ? buildTimestamp : undefined,
    revision,
    download_url,
    artifact: {
      id: artifact.id,
      name: artifact.name,
      size_in_bytes: artifact.size_in_bytes,
      created_at: artifact.created_at,
      updated_at: artifact.updated_at,
      expires_at: artifact.expires_at,
    },
    source,
  };
}

async function resolveReleaseArtifact(
  env: Env,
  kind: ReleaseArtifactKind,
  artifactName: string,
  downloadName: string,
  url: URL,
  options: RequestOptions,
): Promise<ResolvedArtifact> {
  const release = await latestRelease(env);
  const asset = findReleaseAsset(release, artifactName);
  if (!asset) throw new HttpError(`release asset not found: ${artifactName}`, 404);

  const version = normalizeVersion(release.tag_name);
  const revision = await releaseTagRevision(env, release);
  return {
    kind,
    name: asset.name,
    version: kind === 'ant' ? version : undefined,
    revision,
    download_url: downloadUrl(
      url,
      kind,
      downloadName,
      branch(env, options),
      actionsRunId(env, options),
    ),
    artifact: {
      id: asset.id,
      name: asset.name,
      size_in_bytes: asset.size,
      created_at: asset.created_at,
      updated_at: asset.updated_at,
      content_type: asset.content_type,
      api_url: asset.url,
      browser_download_url: asset.browser_download_url,
    },
    source: releaseSourceInfo(release),
  };
}

async function resolveReleaseAsset(
  env: Env,
  release: GitHubRelease,
  kind: ReleaseArtifactKind,
  artifactName: string,
): Promise<ResolvedArtifact> {
  const asset = findReleaseAsset(release, artifactName);
  if (!asset) throw new HttpError(`release asset not found: ${artifactName}`, 404);
  const version = normalizeVersion(release.tag_name);
  const revision = await releaseTagRevision(env, release);

  return {
    kind,
    name: asset.name,
    version: kind === 'ant' ? version : undefined,
    revision,
    download_url: asset.browser_download_url,
    artifact: {
      id: asset.id,
      name: asset.name,
      size_in_bytes: asset.size,
      created_at: asset.created_at,
      updated_at: asset.updated_at,
      content_type: asset.content_type,
      api_url: asset.url,
      browser_download_url: asset.browser_download_url,
    },
    source: releaseSourceInfo(release),
  };
}

async function resolveReleaseAfterActionsMiss(
  env: Env,
  kind: ReleaseArtifactKind,
  artifactName: string,
  downloadName: string,
  url: URL,
  options: RequestOptions,
  actionsReason: string,
): Promise<ResolvedArtifact> {
  try {
    return await resolveReleaseArtifact(env, kind, artifactName, downloadName, url, options);
  } catch (error) {
    if (!isNotFound(error)) throw error;
    const releaseReason = error instanceof Error ? error.message : 'release asset not found';
    throw new HttpError(`${actionsReason}; ${releaseReason}`, 404);
  }
}

function actionSourceInfo(workflow: string, run: WorkflowRun): ActionSourceInfo {
  return {
    type: 'actions',
    repository: GITHUB_REPOSITORY,
    workflow,
    run_id: run.id,
    run_number: run.run_number,
    head_branch: run.head_branch,
    head_sha: run.head_sha,
    html_url: run.html_url,
    created_at: run.created_at,
    updated_at: run.updated_at,
  };
}

function releaseSourceInfo(release: GitHubRelease): ReleaseSourceInfo {
  return {
    type: 'release',
    repository: GITHUB_REPOSITORY,
    release_id: release.id,
    tag_name: release.tag_name,
    name: release.name,
    html_url: release.html_url,
    published_at: release.published_at,
    created_at: release.created_at,
  };
}

function releaseInfo(release: GitHubRelease) {
  return {
    latest: normalizeVersion(release.tag_name),
    tag: release.tag_name,
    source: {
      type: 'release',
      html_url: release.html_url,
      published_at: release.published_at,
    },
  };
}
