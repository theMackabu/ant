import { consoleUrl } from './config';
import { requireToken } from './auth';

export interface Project {
  name: string;
  placement: string;
  active_deployment: string | null;
}

export interface DeployResult {
  deployment: { id: string; status: string; size: number };
  url: string;
  previewUrl: string;
}

async function api(method: string, path: string, init: { json?: unknown } = {}): Promise<Response> {
  const headers: Record<string, string> = { authorization: `Bearer ${requireToken()}` };
  let body: string | undefined;
  if (init.json !== undefined) {
    headers['content-type'] = 'application/json';
    body = JSON.stringify(init.json);
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
  hash: string;
  script: string;
  placement: string;
  observability: boolean;
  vars: Record<string, string>;
  bindings: { kind: string; name: string; id: string; resourceName?: string }[];
  migrations: Record<string, { tag: string; sql: string }[]>;
  assets: { path: string; ct: string; body: string }[];
  assetsConfig: { notFound: string; startAnt: boolean | string[]; binding: string; name?: string } | null;
}

export async function deployManifest(name: string, manifest: DeployManifest): Promise<DeployResult> {
  return asJson<DeployResult>(await api('POST', `/api/projects/${encodeURIComponent(name)}/deploy`, { json: manifest }));
}

export async function deleteProject(name: string): Promise<void> {
  const res = await api('DELETE', `/api/projects/${encodeURIComponent(name)}`);
  if (!res.ok) await asJson(res);
  else await res.body?.cancel();
}
