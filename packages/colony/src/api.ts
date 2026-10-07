import { gzipSync } from 'node:zlib';
import { consoleUrl } from './config';
import { requireToken } from './auth';

export interface Project {
  name: string;
  active: string | null;
}

export interface DeployResult {
  deployment: { id: string; size: number };
  url: string;
  previewUrl: string;
}

// json bodies over a few KB go gzip-compressed (the server accepts
// content-encoding: gzip); raw bodies are sent as-is, see uploadAsset.
async function api(method: string, path: string, init: { json?: unknown; raw?: Uint8Array; headers?: Record<string, string> } = {}): Promise<Response> {
  const headers: Record<string, string> = { authorization: `Bearer ${requireToken()}`, ...init.headers };
  let body: string | Uint8Array | undefined = init.raw;
  if (init.json !== undefined) {
    headers['content-type'] = 'application/json';
    const text = JSON.stringify(init.json);
    if (text.length > 4096) {
      body = gzipSync(text);
      headers['content-encoding'] = 'gzip';
    } else body = text;
  }
  return fetch(`${consoleUrl()}${path}`, { method, headers, body });
}

async function asJson<T>(res: Response): Promise<T> {
  const data = (await res.json().catch(() => null)) as (T & { error?: string; message?: string; modules?: string[] }) | null;
  if (!res.ok) {
    const detail =
      data?.message || (data?.modules ? `${data.error ?? 'Request failed'}: ${data.modules.join(', ')}` : data?.error) || `HTTP ${res.status}`;
    throw new Error(detail);
  }
  if (data === null) throw new Error(`console returned an invalid response (HTTP ${res.status}).`);
  return data;
}

export async function listProjects(): Promise<Project[]> {
  const { projects } = await asJson<{ projects: Project[] }>(await api('GET', '/api/projects'));
  return projects;
}

export async function getProject(name: string): Promise<Project | null> {
  const res = await api('GET', `/api/projects/${encodeURIComponent(name)}`);
  if (res.status === 404) {
    await res.body?.cancel();
    return null;
  }
  const { project } = await asJson<{ project: Project }>(res);
  return project;
}

export interface DeployManifest {
  script: string;
  observability: boolean;
  vars: Record<string, string>;
  bindings: { kind: string; binding: string; name: string }[];
  migrations: Record<string, { tag: string; sql: string }[]>;
  assets: { path: string; ct: string; hash: string }[]; // uploaded first: missingAssets + uploadAsset
  assetsConfig: { notFound: string; startAnt: boolean | string[] } | null;
}

// The hashes this account still has to upload (the server keeps every file it
// was sent, so unchanged ones never travel twice).
export async function missingAssets(hashes: string[]): Promise<string[]> {
  if (!hashes.length) return [];
  const { missing } = await asJson<{ missing: string[] }>(await api('POST', '/api/blobs/missing', { json: { hashes } }));
  return missing;
}

// Uploads one file's raw bytes, gzip-compressed when that makes it smaller
// (text yes, already-compressed images and fonts no). Returns bytes sent.
export async function uploadAsset(hash: string, bytes: Uint8Array): Promise<number> {
  const gz = gzipSync(bytes);
  const useGz = gz.byteLength < bytes.byteLength * 0.9;
  const body = useGz ? gz : bytes;
  const res = await api('PUT', `/api/blobs/${hash}`, {
    raw: body,
    headers: { 'content-type': 'application/octet-stream', ...(useGz ? { 'content-encoding': 'gzip' } : {}) }
  });
  await asJson(res);
  return body.byteLength;
}

export async function deployManifest(name: string, manifest: DeployManifest): Promise<DeployResult> {
  return asJson<DeployResult>(await api('POST', `/api/projects/${encodeURIComponent(name)}/deploy`, { json: manifest }));
}

export async function deleteProject(name: string): Promise<void> {
  const res = await api('DELETE', `/api/projects/${encodeURIComponent(name)}`);
  if (!res.ok) await asJson(res);
  else await res.body?.cancel();
}
