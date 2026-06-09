export type Env = {
  DOWNLOADS: R2Bucket;
  GITHUB_REPOSITORY?: string;
  GITHUB_ACTION_REPOSITORIES?: string;
  GITHUB_RELEASE_REPOSITORY?: string;
  GITHUB_BRANCH?: string;
  GITHUB_RUN_ID?: string;
  GITHUB_TOKEN?: string;
  MANIFEST_REFRESH_TOKEN?: string;
  CACHE_TTL_SECONDS?: string;
};

export type ArtifactKind = 'ant' | 'sandbox' | 'kernel';

export type AntTarget = {
  key: string;
  artifact: string;
  aliases: string[];
  os: 'linux' | 'darwin' | 'windows';
  arch: 'x64' | 'aarch64';
  libc?: 'glibc' | 'musl';
};

export type Artifact = {
  id: number;
  name: string;
  size_in_bytes: number;
  archive_download_url: string;
  expired: boolean;
  created_at: string;
  updated_at: string;
  expires_at: string | null;
  workflow_run?: {
    id: number;
    head_branch: string;
    head_sha: string;
  };
};

export type WorkflowRun = {
  id: number;
  name: string;
  path?: string;
  display_title: string;
  head_branch: string;
  head_sha: string;
  run_number: number;
  event: string;
  status: string;
  conclusion: string | null;
  workflow_id: number;
  html_url: string;
  created_at: string;
  updated_at: string;
};

export type GitHubRunsResponse = {
  workflow_runs: WorkflowRun[];
};

export type GitHubArtifactsResponse = {
  artifacts: Artifact[];
};

export type ReleaseAsset = {
  id: number;
  name: string;
  size: number;
  content_type: string;
  url: string;
  browser_download_url: string;
  created_at: string;
  updated_at: string;
};

export type GitHubRelease = {
  id: number;
  tag_name: string;
  target_commitish?: string;
  name: string | null;
  html_url: string;
  published_at: string | null;
  created_at: string;
  assets: ReleaseAsset[];
};

export type ArtifactInfo = {
  id: number;
  name: string;
  size_in_bytes: number;
  created_at: string;
  updated_at: string;
  expires_at?: string | null;
  content_type?: string;
  api_url?: string;
  browser_download_url?: string;
};

export type ResolvedArtifact = {
  kind: ArtifactKind;
  name: string;
  version?: string;
  revision?: string;
  download_url: string;
  gzip_url?: string;
  gzip_size_in_bytes?: number;
  zip_entry?: string;
  filename?: string;
  artifact: ArtifactInfo;
  source: SourceInfo;
};

export type ActionSourceInfo = {
  type: 'actions';
  repository: string;
  workflow: string;
  run_id: number;
  run_number: number;
  head_branch: string;
  head_sha: string;
  html_url: string;
  created_at: string;
  updated_at: string;
};

export type ReleaseSourceInfo = {
  type: 'release';
  repository: string;
  release_id: number;
  tag_name: string;
  name: string | null;
  html_url: string;
  published_at: string | null;
  created_at: string;
};

export type SourceInfo = ActionSourceInfo | ReleaseSourceInfo;
