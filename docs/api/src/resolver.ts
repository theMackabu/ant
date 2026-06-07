import {
  actionsRunId,
  branch,
  BUILD_WORKFLOW,
  canDownloadActionsArtifacts,
  DEFAULT_BRANCH,
  MUSL_SANDBOX_WORKFLOW,
  releaseRepository,
} from './config';
import type { RequestOptions } from './config';
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

  const sandbox = await Promise.all(
    ['x64', 'aarch64'].map(arch =>
      resolveManifestEntry({ arch }, () => resolveNanosArtifact(env, 'sandbox', arch, url, options)),
    ),
  );

  const kernel = await Promise.all(
    ['x64', 'aarch64'].map(arch =>
      resolveManifestEntry({ arch }, () => resolveNanosArtifact(env, 'kernel', arch, url, options)),
    ),
  );

  return {
    schema: 1,
    generated_at: new Date().toISOString(),
    ant,
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
};

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
) {
  const [release, latest] = await Promise.all([
    latestRelease(env),
    resolveAnt(env, resolveTarget(query.target || ''), url, options),
  ]);
  const current = normalizeVersion(query.current);
  const latestVersion = normalizeVersion(release.tag_name);

  return {
    schema: 1,
    kind: 'ant',
    target: query.target,
    current: current || null,
    latest: latestVersion,
    latest_sha: latest.revision || (latest.source.type === 'actions' ? latest.source.head_sha : null),
    out_of_date: isOutOfDate(current, latestVersion, undefined),
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
    return await resolveActionAnt(env, target, url, options);
  } catch (error) {
    if (!isNotFound(error)) throw error;
    return resolveReleaseArtifact(env, 'ant', target.artifact, target.key, url, options);
  }
}

export async function resolveNamedArtifact(
  env: Env,
  kind: ArtifactKind,
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
    return fetchArtifactDownload(env, artifact.source.repository, artifact.artifact.id);
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

async function resolveActionAnt(
  env: Env,
  target: AntTarget,
  url: URL,
  options: RequestOptions,
): Promise<ResolvedArtifact> {
  const runId = actionsRunId(env, options);
  if (runId) {
    try {
      return await resolveActionAntFromRun(env, target, url, runId, options);
    } catch (error) {
      if (!isNotFound(error)) throw error;
    }
  }

  const match = await findLatestRunWithArtifacts(env, BUILD_WORKFLOW, branch(env, options), [
    target.artifact,
  ]);
  return resolveActionAntFromArtifacts(
    env,
    target,
    url,
    match.repository,
    match.run,
    match.artifacts,
    options,
  );
}

async function resolveActionAntFromRun(
  env: Env,
  target: AntTarget,
  url: URL,
  runId: number,
  options: RequestOptions,
): Promise<ResolvedArtifact> {
  const match = await findRunWithArtifacts(env, runId, [target.artifact]);
  return resolveActionAntFromArtifacts(
    env,
    target,
    url,
    match.repository,
    match.run,
    match.artifacts,
    options,
  );
}

async function resolveActionAntFromArtifacts(
  env: Env,
  target: AntTarget,
  url: URL,
  sourceRepository: string,
  run: WorkflowRun,
  artifacts: Artifact[],
  options: RequestOptions,
): Promise<ResolvedArtifact> {
  const artifact = requireArtifact(artifacts, target.artifact);
  const versionArtifact = artifacts.find(
    item => item.name === `version-${target.artifact}` && !item.expired,
  );
  let version: string | undefined;

  if (versionArtifact) {
    try {
      version = await readVersionArtifact(env, sourceRepository, versionArtifact);
    } catch (error) {
      console.warn(`failed to read ${versionArtifact.name}`, error);
    }
  }

  const resolved = resolvedAction(
    'ant',
    artifact,
    version,
    options.revision || run.head_sha,
    actionSourceInfo(sourceRepository, BUILD_WORKFLOW, run),
    downloadUrl(url, 'ant', target.key, branch(env, options), run.id),
  );

  resolved.zip_entry = target.os === 'windows' ? 'ant.exe' : 'ant';
  resolved.filename = target.os === 'windows' ? 'ant.exe' : 'ant';

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
        options.revision || match.run.head_sha,
        actionSourceInfo(match.repository, match.run.path || workflow, match.run),
        downloadUrl(url, kind, arch, branch(env, options), runId),
      );
    } catch (error) {
      if (!isNotFound(error)) throw error;
    }
  }

  const match = await findLatestRunWithArtifacts(env, workflow, branch(env, options), [artifactName]);
  const artifact = requireArtifact(match.artifacts, artifactName);
  return resolvedAction(
    kind,
    artifact,
    undefined,
    options.revision || match.run.head_sha,
    actionSourceInfo(match.repository, workflow, match.run),
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
    options.revision || match.run.head_sha,
    actionSourceInfo(match.repository, match.run.path || match.run.name, match.run),
    downloadUrl(url, kind, arch, branch(env, options), match.run.id),
  );
}

function resolvedAction(
  kind: ArtifactKind,
  artifact: Artifact,
  version: string | undefined,
  revision: string,
  source: ActionSourceInfo,
  download_url: string,
): ResolvedArtifact {
  return {
    kind,
    name: artifact.name,
    version: kind === 'ant' ? version : undefined,
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
  kind: ArtifactKind,
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
    download_url: downloadUrl(url, kind, downloadName, branch(env, options), actionsRunId(env, options)),
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
    source: releaseSourceInfo(env, release),
  };
}

async function resolveReleaseAsset(
  env: Env,
  release: GitHubRelease,
  kind: ArtifactKind,
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
    source: releaseSourceInfoFromRelease(release, releaseRepository(env)),
  };
}

async function resolveReleaseAfterActionsMiss(
  env: Env,
  kind: ArtifactKind,
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

function actionSourceInfo(repo: string, workflow: string, run: WorkflowRun): ActionSourceInfo {
  return {
    type: 'actions',
    repository: repo,
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

function releaseSourceInfo(env: Env, release: GitHubRelease): ReleaseSourceInfo {
  return releaseSourceInfoFromRelease(release, releaseRepository(env));
}

function releaseSourceInfoFromRelease(
  release: GitHubRelease,
  repo = 'theMackabu/ant',
): ReleaseSourceInfo {
  return {
    type: 'release',
    repository: repo,
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
