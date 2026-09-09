import { GITHUB_REPOSITORY } from './config';
import { HttpError } from './errors';
import { readZipText } from './zip';
import type {
  Artifact,
  Env,
  GitHubArtifactsResponse,
  GitHubRelease,
  GitHubRunsResponse,
  ReleaseAsset,
  WorkflowRun,
} from './types';

type ActionArtifactMatch = {
  repository: string;
  run: WorkflowRun;
  artifacts: Artifact[];
};

type GitHubGitObject = {
  sha: string;
  type: string;
};

type GitHubRef = {
  object: GitHubGitObject;
};

type GitHubTag = {
  object: GitHubGitObject;
};

export type VersionArtifactInfo = {
  version: string;
  buildTimestamp?: number;
};

export async function findLatestRunWithArtifacts(
  env: Env,
  workflow: string,
  branch: string,
  requiredArtifacts: string[],
): Promise<ActionArtifactMatch> {
  const encodedBranch = encodeURIComponent(branch);
  const encodedWorkflow = encodeURIComponent(workflow);

  for (let page = 1; page <= 5; page++) {
    let runs: GitHubRunsResponse;
    try {
      runs = await githubJson<GitHubRunsResponse>(
        env,
        `/repos/${GITHUB_REPOSITORY}/actions/workflows/${encodedWorkflow}/runs?branch=${encodedBranch}&status=success&per_page=20&page=${page}`,
      );
    } catch (error) {
      if (error instanceof HttpError && error.status === 404) break;
      throw error;
    }

    for (const run of runs.workflow_runs) {
      if (run.conclusion !== 'success') continue;
      const artifacts = await listArtifacts(env, run.id);
      const artifactNames = new Set(
        artifacts.filter(artifact => !artifact.expired).map(artifact => artifact.name),
      );
      if (requiredArtifacts.every(name => artifactNames.has(name))) {
        return { repository: GITHUB_REPOSITORY, run, artifacts };
      }
    }
  }

  throw new HttpError(
    `no successful ${workflow} run contains ${requiredArtifacts.join(', ')}`,
    404,
  );
}

export async function findRunWithArtifacts(
  env: Env,
  runId: number,
  requiredArtifacts: string[],
): Promise<ActionArtifactMatch> {
  try {
    const run = await githubJson<WorkflowRun>(env, `/repos/${GITHUB_REPOSITORY}/actions/runs/${runId}`);
    const artifacts = await listArtifacts(env, runId);
    const artifactNames = new Set(
      artifacts.filter(artifact => !artifact.expired).map(artifact => artifact.name),
    );

    if (requiredArtifacts.every(name => artifactNames.has(name))) {
      return { repository: GITHUB_REPOSITORY, run, artifacts };
    }
  } catch (error) {
    if (!(error instanceof HttpError) || error.status !== 404) throw error;
  }

  throw new HttpError(
    `workflow run ${runId} does not contain ${requiredArtifacts.join(', ')}`,
    404,
  );
}

export async function findLatestAnyRunWithArtifacts(
  env: Env,
  requiredArtifacts: string[],
): Promise<ActionArtifactMatch> {
  const matches = await Promise.all(
    requiredArtifacts.map(name => findLatestArtifactByName(env, name)),
  );
  const artifacts = matches.map(match => match.artifact);
  const run = runFromArtifact(artifacts[0]);

  if (artifacts.every(artifact => artifact.workflow_run?.id === run.id)) {
    return { repository: GITHUB_REPOSITORY, run, artifacts };
  }
  throw new HttpError(`no single workflow run contains ${requiredArtifacts.join(', ')}`, 404);
}

export async function findLatestArtifactByName(
  env: Env,
  name: string,
): Promise<{ repository: string; artifact: Artifact }> {
  try {
    const response = await githubJson<GitHubArtifactsResponse>(
      env,
      `/repos/${GITHUB_REPOSITORY}/actions/artifacts?name=${encodeURIComponent(name)}&per_page=20`,
    );
    const artifact = response.artifacts.find(item => item.name === name && !item.expired);
    if (artifact) return { repository: GITHUB_REPOSITORY, artifact };
  } catch (error) {
    if (!(error instanceof HttpError) || error.status !== 404) throw error;
  }

  throw new HttpError(`actions artifact not found: ${name}`, 404);
}

export async function latestRelease(env: Env): Promise<GitHubRelease> {
  return githubJson<GitHubRelease>(env, `/repos/${GITHUB_REPOSITORY}/releases/latest`);
}

export async function releaseByTag(env: Env, version: string): Promise<GitHubRelease> {
  const tag = version.startsWith('v') ? version : `v${version}`;
  return githubJson<GitHubRelease>(
    env,
    `/repos/${GITHUB_REPOSITORY}/releases/tags/${encodeURIComponent(tag)}`,
  );
}

export async function releaseTagRevision(env: Env, release: GitHubRelease): Promise<string> {
  const ref = await githubJson<GitHubRef>(
    env,
    `/repos/${GITHUB_REPOSITORY}/git/ref/tags/${encodeURIComponent(release.tag_name)}`,
  );

  if (ref.object.type === 'commit') return ref.object.sha;
  if (ref.object.type !== 'tag') {
    throw new HttpError(`release tag is not a commit: ${release.tag_name}`, 502);
  }

  const tag = await githubJson<GitHubTag>(
    env,
    `/repos/${GITHUB_REPOSITORY}/git/tags/${encodeURIComponent(ref.object.sha)}`,
  );
  if (tag.object.type !== 'commit') {
    throw new HttpError(`release tag does not point at a commit: ${release.tag_name}`, 502);
  }
  return tag.object.sha;
}

export function findReleaseAsset(
  release: GitHubRelease,
  artifactName: string,
): ReleaseAsset | undefined {
  const candidates = [
    artifactName,
    `${artifactName}.zip`,
    `${artifactName}.tar.gz`,
    `${artifactName}.tgz`,
    `${artifactName}.tar.xz`,
    `${artifactName}.exe`,
  ];

  return (
    release.assets.find(asset => candidates.includes(asset.name)) ||
    release.assets.find(asset => asset.name.startsWith(`${artifactName}.`))
  );
}

export function requireArtifact(artifacts: Artifact[], name: string): Artifact {
  const artifact = artifacts.find(item => item.name === name && !item.expired);
  if (!artifact) throw new HttpError(`artifact not found: ${name}`, 404);
  return artifact;
}

export async function readVersionArtifact(
  env: Env,
  artifact: Artifact,
): Promise<VersionArtifactInfo> {
  const response = await githubFetch(env, artifactApiPath(artifact.id));
  if (!response.ok) throw new HttpError(`failed to download ${artifact.name}`, response.status);

  const zip = await response.arrayBuffer();
  const text = await readZipText(zip, 'version.txt');
  const version = text.trim();
  let buildTimestamp: number | undefined;
  try {
    const timestampText = (await readZipText(zip, 'build-timestamp.txt')).trim();
    if (/^[0-9]+$/.test(timestampText)) buildTimestamp = Number(timestampText);
  } catch (error) {
    if (!(error instanceof HttpError) || !error.message.includes('zip entry not found')) {
      throw error;
    }
  }
  return { version, buildTimestamp };
}

export async function fetchArtifactDownload(
  env: Env,
  artifactId: number,
): Promise<Response> {
  return githubFetch(env, artifactApiPath(artifactId));
}

export async function fetchReleaseAssetDownload(env: Env, assetApiUrl: string): Promise<Response> {
  const url = new URL(assetApiUrl);
  if (
    url.protocol === 'https:' && url.hostname === 'github.com' &&
    url.pathname.startsWith(`/${GITHUB_REPOSITORY}/releases/download/`)
  ) {
    return fetch(assetApiUrl, { redirect: 'follow' });
  }

  return githubFetch(env, assetApiUrl, 'application/octet-stream');
}

function artifactApiPath(artifactId: number): string {
  return `/repos/${GITHUB_REPOSITORY}/actions/artifacts/${artifactId}/zip`;
}

function runFromArtifact(artifact: Artifact): WorkflowRun {
  const workflowRun = artifact.workflow_run;
  if (!workflowRun) throw new HttpError(`artifact missing workflow run: ${artifact.name}`, 502);

  return {
    id: workflowRun.id,
    name: 'unknown',
    display_title: artifact.name,
    head_branch: workflowRun.head_branch,
    head_sha: workflowRun.head_sha,
    run_number: 0,
    event: 'unknown',
    status: 'completed',
    conclusion: 'success',
    workflow_id: 0,
    html_url: `https://github.com/${GITHUB_REPOSITORY}/actions/runs/${workflowRun.id}`,
    created_at: artifact.created_at,
    updated_at: artifact.updated_at,
  };
}

async function listArtifacts(env: Env, runId: number): Promise<Artifact[]> {
  const response = await githubJson<GitHubArtifactsResponse>(
    env,
    `/repos/${GITHUB_REPOSITORY}/actions/runs/${runId}/artifacts?per_page=100`,
  );
  return response.artifacts;
}

async function githubJson<T>(env: Env, path: string): Promise<T> {
  const response = await githubFetch(env, path);
  if (!response.ok) {
    throw new HttpError(`GitHub API request failed: ${response.status}`, response.status);
  }
  return response.json<T>();
}

async function githubFetch(
  env: Env,
  pathOrUrl: string,
  accept = 'application/vnd.github+json',
): Promise<Response> {
  const url = pathOrUrl.startsWith('http') ? pathOrUrl : `https://api.github.com${pathOrUrl}`;
  if (pathOrUrl.startsWith('http')) {
    const parsed = new URL(url);
    if (
      parsed.protocol !== 'https:' || parsed.hostname !== 'api.github.com' ||
      !parsed.pathname.startsWith(`/repos/${GITHUB_REPOSITORY}/`)
    ) {
      throw new HttpError(
        `refusing request outside GitHub repository ${GITHUB_REPOSITORY}`,
        502,
      );
    }
  }
  const headers = new Headers({
    Accept: accept,
    'User-Agent': 'ant-api-worker',
    'X-GitHub-Api-Version': '2022-11-28',
  });

  if (env.GITHUB_TOKEN) headers.set('Authorization', `Bearer ${env.GITHUB_TOKEN}`);
  return fetch(url, { headers, redirect: 'follow' });
}
