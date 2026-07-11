import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { exec } from './utils';

type PackResult = {
  name: string;
  version: string;
  size: number;
  shasum: string;
  integrity: string;
  filename: string;
  entryCount: number;
};

type UploadPart = { partNumber: number; etag: string };

function parsePackResult(stdout: string): PackResult[] {
  for (let at = stdout.lastIndexOf('['); at >= 0; at = stdout.lastIndexOf('[', at - 1)) {
    try {
      const value = JSON.parse(stdout.slice(at)) as unknown;
      if (Array.isArray(value)) return value as PackResult[];
    } catch {}
  }
  throw new Error('npm pack returned an invalid result');
}

function publishTag(args: string[]): string {
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--tag' && args[i + 1]) return args[i + 1];
    if (args[i].startsWith('--tag=')) return args[i].slice('--tag='.length);
  }
  return 'latest';
}

function isDryRun(args: string[]): boolean {
  return args.includes('--dry-run');
}

async function requestJson<T>(url: string, token: string, init: RequestInit): Promise<T> {
  const headers = new Headers(init.headers);
  headers.set('authorization', `Bearer ${token}`);
  const res = await fetch(url, { ...init, headers });
  const text = await res.text();
  let body: unknown = null;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    body = text;
  }
  if (!res.ok) {
    const detail = body && typeof body === 'object' && 'error' in body ? String((body as { error: unknown }).error) : text;
    throw new Error(`publish request failed with HTTP ${res.status}${detail ? `: ${detail}` : ''}`);
  }
  return body as T;
}

function readReadme(dir: string): Promise<string | undefined> {
  const names = ['README.md', 'README', 'readme.md', 'Readme.md'];
  return (async () => {
    for (const name of names) {
      try {
        return await fs.promises.readFile(path.join(dir, name), 'utf8');
      } catch (err) {
        if ((err as NodeJS.ErrnoException).code !== 'ENOENT') throw err;
      }
    }
    return undefined;
  })();
}

export async function multipartPublish(registryUrl: string, token: string | null, publishArgs: string[]): Promise<void> {
  const cwd = process.cwd();
  const temp = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'antland-publish-'));
  let uploadUrl: string | null = null;
  let finalized = false;
  try {
    const packed = await exec('npm', ['pack', '--json', '--pack-destination', temp], cwd, process.env, true);
    const parsed = parsePackResult(packed.stdout);
    const info = parsed[0];
    if (!info?.name || !info.version || !info.filename || !info.shasum || !info.integrity) throw new Error('npm pack returned an invalid result');
    const tarball = path.join(temp, info.filename);
    const stat = await fs.promises.stat(tarball);

    console.log(`Packed ${info.entryCount} files into ${info.filename} (${stat.size} bytes)`);
    if (isDryRun(publishArgs)) {
      console.log('Dry run complete. No upload performed.');
      return;
    }
    if (!token) throw new Error('No ants.land token found. Run antland login first.');

    const base = registryUrl.replace(/\/+$/, '');
    const created = await requestJson<{ id: string; partSize: number }>(`${base}/-/v1/publish/uploads`, token, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name: info.name, version: info.version, size: stat.size, shasum: info.shasum, integrity: info.integrity })
    });
    uploadUrl = `${base}/-/v1/publish/uploads/${encodeURIComponent(created.id)}`;
    const partSize = created.partSize;
    if (!Number.isSafeInteger(partSize) || partSize < 5 * 1024 * 1024) throw new Error('registry returned an invalid multipart part size');

    const parts: UploadPart[] = [];
    const file = await fs.promises.open(tarball, 'r');
    try {
      let offset = 0;
      let partNumber = 1;
      while (offset < stat.size) {
        const length = Math.min(partSize, stat.size - offset);
        const bytes = Buffer.allocUnsafe(length);
        const { bytesRead } = await file.read(bytes, 0, length, offset);
        if (bytesRead !== length) throw new Error('tarball changed while it was being uploaded');
        const part = await requestJson<UploadPart>(`${uploadUrl}/parts/${partNumber}`, token, {
          method: 'PUT',
          headers: { 'content-type': 'application/octet-stream', 'content-length': String(length) },
          body: bytes
        });
        parts.push(part);
        offset += length;
        partNumber++;
      }
    } finally {
      await file.close();
    }

    await requestJson(`${uploadUrl}/complete`, token, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ parts })
    });

    const manifest = JSON.parse(await fs.promises.readFile(path.join(cwd, 'package.json'), 'utf8')) as Record<string, unknown>;
    const readme = await readReadme(cwd);
    const metadata: Record<string, unknown> = {
      _id: info.name,
      name: info.name,
      'dist-tags': { [publishTag(publishArgs)]: info.version },
      versions: { [info.version]: { ...manifest, name: info.name, version: info.version } }
    };
    if (readme) metadata.readme = readme;
    await requestJson(`${uploadUrl}/finalize`, token, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(metadata)
    });
    finalized = true;
    console.log(`Published ${info.name}@${info.version}`);
  } finally {
    if (uploadUrl && !finalized) {
      await fetch(uploadUrl, { method: 'DELETE', headers: { authorization: `Bearer ${token}` } }).catch(() => {});
    }
    await fs.promises.rm(temp, { recursive: true, force: true });
  }
}
