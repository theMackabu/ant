import { actionRepositories, releaseRepository } from './config';
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

export async function findLatestRunWithArtifacts(
  env: Env,
  workflow: string,
  branch: string,
  requiredArtifacts: string[],
): Promise<ActionArtifactMatch> {
  const candidates: ActionArtifactMatch[] = [];
  const encodedBranch = encodeURIComponent(branch);
  const encodedWorkflow = encodeURIComponent(workflow);

  for (const repo of actionRepositories(env)) {
    for (let page = 1; page <= 5; page++) {
      let runs: GitHubRunsResponse;
      try {
        runs = await githubJson<GitHubRunsResponse>(
          env,
          `/repos/${repo}/actions/workflows/${encodedWorkflow}/runs?branch=${encodedBranch}&status=success&per_page=20&page=${page}`,
        );
      } catch (error) {
        if (error instanceof HttpError && error.status === 404) break;
        throw error;
      }

      let found = false;
      for (const run of runs.workflow_runs) {
        if (run.conclusion !== 'success') continue;
        const artifacts = await listArtifacts(env, repo, run.id);
        const artifactNames = new Set(
          artifacts.filter(artifact => !artifact.expired).map(artifact => artifact.name),
        );
        if (requiredArtifacts.every(name => artifactNames.has(name))) {
          candidates.push({ repository: repo, run, artifacts });
          found = true;
          break;
        }
      }

      if (found) break;
    }
  }

  const latest = candidates.sort(
    (a, b) => Date.parse(b.run.created_at) - Date.parse(a.run.created_at),
  )[0];
  if (latest) return latest;

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
  for (const repo of actionRepositories(env)) {
    try {
      const run = await githubJson<WorkflowRun>(env, `/repos/${repo}/actions/runs/${runId}`);
      const artifacts = await listArtifacts(env, repo, runId);
      const artifactNames = new Set(
        artifacts.filter(artifact => !artifact.expired).map(artifact => artifact.name),
      );

      if (requiredArtifacts.every(name => artifactNames.has(name))) {
        return { repository: repo, run, artifacts };
      }
    } catch (error) {
      if (error instanceof HttpError && error.status === 404) continue;
      throw error;
    }
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
  const { repository: repo } = matches[0];
  const artifacts = matches.map(match => match.artifact);
  const run = runFromArtifact(repo, artifacts[0]);

  if (
    matches.every(match => match.repository === repo) &&
    artifacts.every(artifact => artifact.workflow_run?.id === run.id)
  ) {
    return { repository: repo, run, artifacts };
  }
  throw new HttpError(`no single workflow run contains ${requiredArtifacts.join(', ')}`, 404);
}

export async function findLatestArtifactByName(
  env: Env,
  name: string,
): Promise<{ repository: string; artifact: Artifact }> {
  const candidates: { repository: string; artifact: Artifact }[] = [];

  for (const repo of actionRepositories(env)) {
    let response: GitHubArtifactsResponse;
    try {
      response = await githubJson<GitHubArtifactsResponse>(
        env,
        `/repos/${repo}/actions/artifacts?name=${encodeURIComponent(name)}&per_page=20`,
      );
    } catch (error) {
      if (error instanceof HttpError && error.status === 404) continue;
      throw error;
    }
    const artifact = response.artifacts.find(item => item.name === name && !item.expired);
    if (artifact) candidates.push({ repository: repo, artifact });
  }

  const latest = candidates.sort(
    (a, b) => Date.parse(b.artifact.created_at) - Date.parse(a.artifact.created_at),
  )[0];
  if (latest) return latest;

  throw new HttpError(`actions artifact not found: ${name}`, 404);
}

export async function latestRelease(env: Env): Promise<GitHubRelease> {
  return githubJson<GitHubRelease>(env, `/repos/${releaseRepository(env)}/releases/latest`);
}

export async function releaseByTag(env: Env, version: string): Promise<GitHubRelease> {
  const tag = version.startsWith('v') ? version : `v${version}`;
  return githubJson<GitHubRelease>(
    env,
    `/repos/${releaseRepository(env)}/releases/tags/${encodeURIComponent(tag)}`,
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

export async function readVersionArtifact(
  env: Env,
  repo: string,
  artifact: Artifact,
): Promise<string> {
  const response = await githubFetch(env, artifactApiPath(repo, artifact.id));
  if (!response.ok) throw new HttpError(`failed to download ${artifact.name}`, response.status);

  const zip = await response.arrayBuffer();
  const text = await readZipText(zip, 'version.txt');
  return text.trim();
}

export async function fetchArtifactDownload(
  env: Env,
  repo: string,
  artifactId: number,
): Promise<Response> {
  return githubFetch(env, artifactApiPath(repo, artifactId));
}

export async function fetchReleaseAssetDownload(env: Env, assetApiUrl: string): Promise<Response> {
  if (!assetApiUrl.includes('/repos/') && !assetApiUrl.includes('/releases/assets/')) {
    return fetch(assetApiUrl, { redirect: 'follow' });
  }

  return githubFetch(env, assetApiUrl, 'application/octet-stream');
}

function artifactApiPath(repo: string, artifactId: number): string {
  return `/repos/${repo}/actions/artifacts/${artifactId}/zip`;
}

function runFromArtifact(repo: string, artifact: Artifact): WorkflowRun {
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
    html_url: `https://github.com/${repo}/actions/runs/${workflowRun.id}`,
    created_at: artifact.created_at,
    updated_at: artifact.updated_at,
  };
}

async function listArtifacts(env: Env, repo: string, runId: number): Promise<Artifact[]> {
  const response = await githubJson<GitHubArtifactsResponse>(
    env,
    `/repos/${repo}/actions/runs/${runId}/artifacts?per_page=100`,
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
    if (parsed.protocol !== 'https:' || parsed.hostname !== 'api.github.com') {
      throw new HttpError(
        `refusing authenticated request to untrusted GitHub host: ${parsed.hostname}`,
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
