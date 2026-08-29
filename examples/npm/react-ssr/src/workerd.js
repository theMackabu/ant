import { join } from 'node:path';
import { gunzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { chmod, mkdir, rename, rm, stat, writeFile } from 'node:fs/promises';

const release = 'v1.20260828.1';
const targets = {
  'darwin-x64': {
    name: 'darwin-64',
    sha256: 'ad0f45b10df7062c11ebbd7566d43a24cf25fca159f04b5a5283bb5e16cca9c9'
  },
  'darwin-arm64': {
    name: 'darwin-arm64',
    sha256: 'bf33617c574d90dde4c415d63a5d0197c50ef3ae88f1d26809333f70343cb92e'
  },
  'linux-x64': {
    name: 'linux-64',
    sha256: 'acbfd0a302846bc9be58f67037492e46984fd685d55960dda5ce1e36c199681e'
  },
  'linux-arm64': {
    name: 'linux-arm64',
    sha256: '43e4cdd170e55c8a9d31b5aeb50e66f8f727d32246b519e034ddc2977fca810c'
  },
  'win32-x64': {
    name: 'windows-64',
    sha256: 'f7d0511b1f2b1cf81643b05c7af35ebcc171c2252f948ae9c8011ae254ac97d0'
  }
};

const platform = `${process.platform}-${process.arch}`;
const target = targets[platform];

if (!target) {
  throw new Error(`workerd ${release} does not provide a binary for ${platform}`);
}

const root = import.meta.dirname;
const binDirectory = join(root, 'bin');
const extension = process.platform === 'win32' ? '.exe' : '';
const binaryPath = join(binDirectory, `workerd-${target.name}-${release}${extension}`);

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch (error) {
    if (error?.code === 'ENOENT') return false;
    throw error;
  }
}

async function download() {
  const asset = `workerd-${target.name}.gz`;
  const url = `https://github.com/cloudflare/workerd/releases/download/${release}/${asset}`;

  console.log(`Downloading ${asset} from workerd ${release}...`);
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to download ${url}: HTTP ${response.status}`);
  }

  const archive = new Uint8Array(await response.arrayBuffer());
  const digest = createHash('sha256').update(archive).digest('hex');
  if (digest !== target.sha256) {
    throw new Error(`SHA-256 mismatch for ${asset}: expected ${target.sha256}, received ${digest}`);
  }

  const temporaryPath = `${binaryPath}.tmp-${process.pid}`;
  await mkdir(binDirectory, { recursive: true });

  try {
    await writeFile(temporaryPath, gunzipSync(archive), { mode: 0o755 });
    if (process.platform !== 'win32') await chmod(temporaryPath, 0o755);
    await rename(temporaryPath, binaryPath);
  } catch (error) {
    await rm(temporaryPath, { force: true });
    throw error;
  }
}

await import('../build.js');

if (!(await exists(binaryPath))) await download();
const child = spawn(binaryPath, ['serve', join(root, 'config.capnp'), 'helloWorldExample'], { stdio: 'inherit' });

const result = await new Promise((resolve, reject) => {
  child.once('error', reject);
  child.once('exit', (code, signal) => resolve({ code, signal }));
});

if (result.signal) process.kill(process.pid, result.signal);
else process.exitCode = result.code ?? 1;
