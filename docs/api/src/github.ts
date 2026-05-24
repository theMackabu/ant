import { repository } from './config';
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

export async function findLatestRunWithArtifacts(
  env: Env,
  workflow: string,
  branch: string,
  requiredArtifacts: string[],
): Promise<{ run: WorkflowRun; artifacts: Artifact[] }> {
  const repo = repository(env);
  const encodedBranch = encodeURIComponent(branch);
  const encodedWorkflow = encodeURIComponent(workflow);

  for (let page = 1; page <= 5; page++) {
    const runs = await githubJson<GitHubRunsResponse>(
      env,
      `/repos/${repo}/actions/workflows/${encodedWorkflow}/runs?branch=${encodedBranch}&status=success&per_page=20&page=${page}`,
    );

    for (const run of runs.workflow_runs) {
      if (run.conclusion !== 'success') continue;
      const artifacts = await listArtifacts(env, run.id);
      const artifactNames = new Set(
        artifacts.filter(artifact => !artifact.expired).map(artifact => artifact.name),
      );
      if (requiredArtifacts.every(name => artifactNames.has(name))) return { run, artifacts };
    }
  }

  throw new HttpError(
    `no successful ${workflow} run contains ${requiredArtifacts.join(', ')}`,
    404,
  );
}

export async function findLatestAnyRunWithArtifacts(
  env: Env,
  requiredArtifacts: string[],
): Promise<{ run: WorkflowRun; artifacts: Artifact[] }> {
  const artifacts = await Promise.all(
    requiredArtifacts.map(name => findLatestArtifactByName(env, name)),
  );
  const run = runFromArtifact(env, artifacts[0]);

  if (artifacts.every(artifact => artifact.workflow_run?.id === run.id)) return { run, artifacts };
  throw new HttpError(`no single workflow run contains ${requiredArtifacts.join(', ')}`, 404);
}

export async function findLatestArtifactByName(env: Env, name: string): Promise<Artifact> {
  const response = await githubJson<GitHubArtifactsResponse>(
    env,
    `/repos/${repository(env)}/actions/artifacts?name=${encodeURIComponent(name)}&per_page=20`,
  );
  const artifact = response.artifacts.find(item => item.name === name && !item.expired);
  if (!artifact) throw new HttpError(`actions artifact not found: ${name}`, 404);
  return artifact;
}

export async function latestRelease(env: Env): Promise<GitHubRelease> {
  return githubJson<GitHubRelease>(env, `/repos/${repository(env)}/releases/latest`);
}

export async function releaseByTag(env: Env, version: string): Promise<GitHubRelease> {
  const tag = version.startsWith('v') ? version : `v${version}`;
  return githubJson<GitHubRelease>(
    env,
    `/repos/${repository(env)}/releases/tags/${encodeURIComponent(tag)}`,
  );
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

export async function readVersionArtifact(env: Env, artifact: Artifact): Promise<string> {
  const response = await githubFetch(env, artifactApiPath(env, artifact.id));
  if (!response.ok) throw new HttpError(`failed to download ${artifact.name}`, response.status);

  const zip = await response.arrayBuffer();
  const text = await readZipText(zip, 'version.txt');
  return text.trim();
}

export async function fetchArtifactDownload(env: Env, artifactId: number): Promise<Response> {
  return githubFetch(env, artifactApiPath(env, artifactId));
}

export async function fetchReleaseAssetDownload(env: Env, assetApiUrl: string): Promise<Response> {
  if (!assetApiUrl.includes('/repos/') && !assetApiUrl.includes('/releases/assets/')) {
    return fetch(assetApiUrl, { redirect: 'follow' });
  }

  return githubFetch(env, assetApiUrl, 'application/octet-stream');
}

function artifactApiPath(env: Env, artifactId: number): string {
  return `/repos/${repository(env)}/actions/artifacts/${artifactId}/zip`;
}

function runFromArtifact(env: Env, artifact: Artifact): WorkflowRun {
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
    html_url: `https://github.com/${repository(env)}/actions/runs/${workflowRun.id}`,
    created_at: artifact.created_at,
    updated_at: artifact.updated_at,
  };
}

async function listArtifacts(env: Env, runId: number): Promise<Artifact[]> {
  const response = await githubJson<GitHubArtifactsResponse>(
    env,
    `/repos/${repository(env)}/actions/runs/${runId}/artifacts?per_page=100`,
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
  const headers = new Headers({
    Accept: accept,
    'User-Agent': 'ant-api-worker',
    'X-GitHub-Api-Version': '2022-11-28',
  });

  if (env.GITHUB_TOKEN) headers.set('Authorization', `Bearer ${env.GITHUB_TOKEN}`);
  return fetch(url, { headers, redirect: 'follow' });
}
