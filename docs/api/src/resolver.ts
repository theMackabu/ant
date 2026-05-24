import {
  branch,
  BUILD_WORKFLOW,
  canDownloadActionsArtifacts,
  DEFAULT_BRANCH,
  MUSL_SANDBOX_WORKFLOW,
  repository,
} from './config';
import { HttpError, isNotFound } from './errors';
import {
  fetchArtifactDownload,
  fetchReleaseAssetDownload,
  findLatestAnyRunWithArtifacts,
  findLatestRunWithArtifacts,
  findReleaseAsset,
  latestRelease,
  releaseByTag,
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

export async function latestManifest(url: URL, env: Env) {
  const ant = await Promise.all(
    antTargets.map(target =>
      resolveManifestEntry(
        {
          target: target.key,
          os: target.os,
          arch: target.arch,
          libc: target.libc,
        },
        () => resolveAnt(env, target, url),
      ),
    ),
  );

  const sandbox = await Promise.all(
    ['x64', 'aarch64'].map(arch =>
      resolveManifestEntry({ arch }, () => resolveNanosArtifact(env, 'sandbox', arch, url)),
    ),
  );

  const kernel = await Promise.all(
    ['x64', 'aarch64'].map(arch =>
      resolveManifestEntry({ arch }, () => resolveNanosArtifact(env, 'kernel', arch, url)),
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
  kind: ArtifactKind;
  target?: string;
  arch?: string;
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

export async function versionCheck(url: URL, env: Env, query: VersionCheckQuery) {
  const [release, latest] = await Promise.all([
    latestRelease(env),
    query.kind === 'ant'
      ? await resolveAnt(env, resolveTarget(query.target || ''), url)
      : await resolveNanosArtifact(env, query.kind, resolveVersionArch(query), url),
  ]);
  const current = normalizeVersion(query.current);
  const latestVersion = normalizeVersion(release.tag_name);

  return {
    schema: 1,
    kind: query.kind,
    target: query.kind === 'ant' ? query.target : `${query.kind}-${resolveVersionArch(query)}`,
    current: current || null,
    latest: latestVersion,
    latest_sha: latest.source.type === 'actions' ? latest.source.head_sha : null,
    out_of_date: isOutOfDate(current, latestVersion, undefined),
    download_url: latest.download_url,
    source: latest.source,
  };
}

function resolveVersionArch(query: VersionCheckQuery): string {
  const raw = query.arch || (query.target || '').replace(/^(sandbox|kernel)-/, '');
  if (raw === 'x64' || raw === 'aarch64') return raw;
  throw new HttpError(`unknown arch: ${raw || '(missing)'}`, 400);
}

export async function resolveAnt(env: Env, target: AntTarget, url: URL): Promise<ResolvedArtifact> {
  if (!canDownloadActionsArtifacts(env)) {
    return resolveReleaseArtifact(env, 'ant', target.artifact, target.key, url);
  }

  try {
    return await resolveActionAnt(env, target, url);
  } catch (error) {
    if (!isNotFound(error)) throw error;
    return resolveReleaseArtifact(env, 'ant', target.artifact, target.key, url);
  }
}

export async function resolveNamedArtifact(
  env: Env,
  kind: ArtifactKind,
  workflow: string,
  artifactName: string,
  url: URL,
): Promise<ResolvedArtifact> {
  const arch = artifactName.endsWith('aarch64') ? 'aarch64' : 'x64';

  if (!canDownloadActionsArtifacts(env)) {
    return resolveReleaseAfterActionsMiss(
      env,
      kind,
      artifactName,
      arch,
      url,
      'actions artifact lookup skipped because GITHUB_TOKEN is not configured',
    );
  }

  try {
    return await resolveActionNamedArtifact(env, kind, workflow, artifactName, arch, url);
  } catch (error) {
    if (!isNotFound(error)) throw error;
    const primaryReason = error instanceof Error ? error.message : 'actions artifact not found';

    try {
      return await resolveAnyActionNamedArtifact(env, kind, artifactName, arch, url);
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
): Promise<ResolvedArtifact> {
  const artifact = await resolveNamedArtifact(
    env,
    kind,
    MUSL_SANDBOX_WORKFLOW,
    `ant-sandbox-${arch}`,
    url,
  );

  const filename = kind === 'kernel' ? `ant-kernel-${arch}.img` : `ant-sandbox-${arch}.img`;
  return {
    ...artifact,
    name: `${kind === 'kernel' ? 'ant-kernel' : 'ant-sandbox'}-${arch}`,
    zip_entry: `packages/sandbox/${filename}`,
    filename,
  };
}

export async function fetchDownload(env: Env, artifact: ResolvedArtifact): Promise<Response> {
  if (artifact.source.type === 'actions') return fetchArtifactDownload(env, artifact.artifact.id);

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
): string {
  const next = new URL(`/v1/download/${kind}/${encodeURIComponent(name)}`, url);
  if (url.searchParams.has('branch') || artifactBranch !== DEFAULT_BRANCH) {
    next.searchParams.set('branch', artifactBranch);
  }
  return next.toString();
}

async function resolveActionAnt(env: Env, target: AntTarget, url: URL): Promise<ResolvedArtifact> {
  const { run, artifacts } = await findLatestRunWithArtifacts(env, BUILD_WORKFLOW, branch(env), [
    target.artifact,
  ]);
  const artifact = requireArtifact(artifacts, target.artifact);
  const versionArtifact = artifacts.find(
    item => item.name === `version-${target.artifact}` && !item.expired,
  );
  let version: string | undefined;

  if (versionArtifact) {
    try {
      version = await readVersionArtifact(env, versionArtifact);
    } catch (error) {
      console.warn(`failed to read ${versionArtifact.name}`, error);
    }
  }

  return resolvedAction(
    'ant',
    artifact,
    version,
    actionSourceInfo(env, BUILD_WORKFLOW, run),
    downloadUrl(url, 'ant', target.key, branch(env)),
  );
}

async function resolveActionNamedArtifact(
  env: Env,
  kind: ArtifactKind,
  workflow: string,
  artifactName: string,
  arch: string,
  url: URL,
): Promise<ResolvedArtifact> {
  const { run, artifacts } = await findLatestRunWithArtifacts(env, workflow, branch(env), [
    artifactName,
  ]);
  const artifact = requireArtifact(artifacts, artifactName);
  return resolvedAction(
    kind,
    artifact,
    undefined,
    actionSourceInfo(env, workflow, run),
    downloadUrl(url, kind, arch, branch(env)),
  );
}

async function resolveAnyActionNamedArtifact(
  env: Env,
  kind: ArtifactKind,
  artifactName: string,
  arch: string,
  url: URL,
): Promise<ResolvedArtifact> {
  const { run, artifacts } = await findLatestAnyRunWithArtifacts(env, [artifactName]);
  const artifact = requireArtifact(artifacts, artifactName);
  return resolvedAction(
    kind,
    artifact,
    undefined,
    actionSourceInfo(env, run.path || run.name, run),
    downloadUrl(url, kind, arch, branch(env)),
  );
}

function resolvedAction(
  kind: ArtifactKind,
  artifact: Artifact,
  version: string | undefined,
  source: ActionSourceInfo,
  download_url: string,
): ResolvedArtifact {
  return {
    kind,
    name: artifact.name,
    version,
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
): Promise<ResolvedArtifact> {
  const release = await latestRelease(env);
  const asset = findReleaseAsset(release, artifactName);
  if (!asset) throw new HttpError(`release asset not found: ${artifactName}`, 404);

  const version = kind === 'ant' ? normalizeVersion(release.tag_name) : undefined;
  return {
    kind,
    name: asset.name,
    version,
    download_url: downloadUrl(url, kind, downloadName, branch(env)),
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

function resolveReleaseAsset(
  env: Env,
  release: GitHubRelease,
  kind: ArtifactKind,
  artifactName: string,
): ResolvedArtifact {
  const asset = findReleaseAsset(release, artifactName);
  if (!asset) throw new HttpError(`release asset not found: ${artifactName}`, 404);

  return {
    kind,
    name: asset.name,
    version: normalizeVersion(release.tag_name),
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
    source: releaseSourceInfoFromRelease(release, repository(env)),
  };
}

async function resolveReleaseAfterActionsMiss(
  env: Env,
  kind: ArtifactKind,
  artifactName: string,
  downloadName: string,
  url: URL,
  actionsReason: string,
): Promise<ResolvedArtifact> {
  try {
    return await resolveReleaseArtifact(env, kind, artifactName, downloadName, url);
  } catch (error) {
    if (!isNotFound(error)) throw error;
    const releaseReason = error instanceof Error ? error.message : 'release asset not found';
    throw new HttpError(`${actionsReason}; ${releaseReason}`, 404);
  }
}

function actionSourceInfo(env: Env, workflow: string, run: WorkflowRun): ActionSourceInfo {
  return {
    type: 'actions',
    repository: repository(env),
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
  return releaseSourceInfoFromRelease(release, repository(env));
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
